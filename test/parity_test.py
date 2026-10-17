#!/usr/bin/env python3
"""Vanilla connection/survival rules: ping, keep-alive, swing + skin data seen by others, movement checks, armor,
game rules. Usage: python3 test/parity_test.py host port  (flat test world, Sam is op)"""
import json, os, struct, sys, time, uuid
sys.path.insert(0, os.path.dirname(__file__))
from bot import *

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ITEMS = json.load(open(os.path.join(ROOT, "data/reports/registries.json")))["minecraft:item"]["entries"]
def item(n): return ITEMS["minecraft:" + n]["protocol_id"]
def cmd(c, s): c.send(PLAY_SB["chat_command"], string(s))
def health(c, pred):
    return struct.unpack(">f", wait(c, PLAY_CB["set_health"], lambda b: pred(struct.unpack(">f", b.d[b.i:b.i + 4])[0])).take(4))[0]

host, port = sys.argv[1], int(sys.argv[2])
for attempt in range(20):  # survival.py's Sam may still be leaving
    try:
        sam, _ = login("Sam", host, port, view=2, uid=uuid.uuid3(uuid.NAMESPACE_DNS, "Sam"))
        break
    except AssertionError:
        time.sleep(0.2)
cmd(sam, "gamemode creative")
eve, _ = login("Eve", host, port, view=2)

# ping_request -> pong_response with the same payload
sam.send(PLAY_SB["ping_request"], struct.pack(">q", 0x1234567890))
assert wait(sam, PLAY_CB["pong_response"]).i64() == 0x1234567890
print("ping ok")

# Punch: the others see the arm swing (swing_animation main hand, whack, 6 ticks); skin layers arrive as entity data.
sam.send(PLAY_SB["punch"])
sw = wait(eve, PLAY_CB["swing_animation"])
sw.varint()
assert (sw.varint(), sw.varint(), sw.varint()) == (0, 1, 6)
print("swing ok")
sam.send(PLAY_SB["client_information"], string("en_us") + bytes([2]) + varint(0) + b"\x01" + bytes([0x3f]) + varint(0) + b"\x00\x01" + varint(0))
def skin_is(b, want):
    b.varint()
    while True:
        idx = b.u8()
        if idx == 0xFF: return False
        t = b.varint()
        v = b.u8() if t == 0 else b.varint()
        if idx == 16: return v == want
d = wait(eve, PLAY_CB["set_entity_data"], lambda b: skin_is(b, 0x3f))
print("skin layers ok")

# Sneaking shows as the crouching pose (6 = 5) and shared flag 0x02.
sam.send(PLAY_SB["player_input"], b"\x20")
def crouching(b):
    b.varint(); flags = pose = None
    while (idx := b.u8()) != 0xFF:
        t = b.varint(); v = b.u8() if t == 0 else b.varint()
        if idx == 0: flags = v
        if idx == 6: pose = v
    return flags == 2 and pose == 5
wait(eve, PLAY_CB["set_entity_data"], crouching)
sam.send(PLAY_SB["player_input"], b"\x00")
print("crouch pose ok")

# Moving 20 blocks in one packet: "moved too quickly", teleported back.
x0, y0, z0 = sam.pos
sam.send(PLAY_SB["move_player_pos"], struct.pack(">dddB", x0 + 20, y0, z0, 1))
tp = wait(sam, PLAY_CB["player_position"])
tp.varint()
assert struct.unpack(">ddd", tp.take(24)) == (x0, y0, z0)
print("moved too quickly ok")

# Armor: 10 mob_attack damage through a diamond chestplate (8 armor, 2 toughness) -> 8.4.
sam.send(PLAY_SB["set_creative_mode_slot"], struct.pack(">h", 6) + varint(1) + varint(item("diamond_chestplate")) + varint(0) + varint(0))
cmd(sam, "gamemode survival")
wait(sam, PLAY_CB["game_event"], lambda b: b.d[b.i] == 3)
cmd(sam, "damage @s 10 minecraft:mob_attack")
h = health(sam, lambda v: v < 20)
assert abs(h - 11.6) < 1e-4, h
print("armor ok:", h)

# fall_damage false: a 10 block fall does nothing; keep_inventory keeps the chestplate through death.
cmd(sam, "gamerule fall_damage false")
cmd(sam, "gamerule keep_inventory true")
time.sleep(0.2)
walk(sam, x0, y0 + 10, z0, False, step=3.0)
walk(sam, x0, y0, z0, True, step=3.0)
time.sleep(0.3)
cmd(sam, "kill @s")
wait(sam, PLAY_CB["player_combat_kill"])
sam.send(PLAY_SB["client_command"], varint(0))
wait(sam, PLAY_CB["respawn"])
c = wait(sam, PLAY_CB["container_set_content"])
c.varint(); c.varint(); c.varint()
slots = []
for _ in range(46):
    n = c.varint()
    if n: slots.append(c.varint()); [c.varint() for _ in range(2)]
    else: slots.append(-1)
assert slots[6] == item("diamond_chestplate"), slots[:9]
cmd(sam, "gamerule fall_damage true")
cmd(sam, "gamerule keep_inventory false")
cmd(sam, "clear @s")
cmd(sam, "gamemode creative")
print("game rules ok")

# NaN position: disconnected with multiplayer.disconnect.invalid_player_movement
eve.send(PLAY_SB["move_player_pos"], struct.pack(">dddB", float("nan"), 0, 0, 1))
d = wait(eve, PLAY_CB["disconnect"])
assert b"multiplayer.disconnect.invalid_player_movement" in d.d
print("invalid movement disconnect ok")
sam.s.close()
