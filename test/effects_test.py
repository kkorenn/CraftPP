#!/usr/bin/env python3
"""Mob effects, potions and enchantments: /effect -> update_mob_effect + speed attribute, golden apple, milk,
resistance. Usage: python3 test/effects_test.py host port  (flat test world, Sam is op)"""
import json, os, struct, sys, time, uuid
sys.path.insert(0, os.path.dirname(__file__))
from bot import *
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REG = json.load(open(os.path.join(ROOT, "vanilla/generated/reports/registries.json")))
def rid(reg, n): return REG["minecraft:" + reg]["entries"]["minecraft:" + n]["protocol_id"]
SPEED, REGEN, ABSORB, RESIST = (rid("mob_effect", n) for n in ("speed", "regeneration", "absorption", "resistance"))
MOVE_SPEED = rid("attribute", "movement_speed")

def cmd(c, s): c.send(PLAY_SB["chat_command"], string(s))

def effect_added(c, effect):
    """update_mob_effect for `effect`: (amplifier, duration, flags)."""
    def is_it(b):
        c.eid = b.varint()
        return b.varint() == effect
    b = wait(c, PLAY_CB["update_mob_effect"], is_it)
    return b.varint(), b.varint(), b.u8()

def removed(c, effect):
    wait(c, PLAY_CB["remove_mob_effect"], lambda b: (b.varint(), b.varint())[1] == effect)

def speed_modifier(c):
    """update_attributes for movement_speed: the minecraft:effect.speed modifier amount (None when absent)."""
    while True:
        b = wait(c, PLAY_CB["update_attributes"], lambda b: b.varint() == c.eid)
        for _ in range(b.varint()):
            a = b.varint(); b.take(8)
            mods = {}
            for _ in range(b.varint()):
                mid = b.str(); amount = struct.unpack(">d", b.take(8))[0]; b.varint()
                mods[mid] = amount
            if a == MOVE_SPEED: return mods.get("minecraft:effect.speed")

def health(c):
    return struct.unpack(">f", wait(c, PLAY_CB["set_health"]).take(4))[0]

host, port = sys.argv[1], int(sys.argv[2])
for attempt in range(50):  # the previous test's Sam may still be disconnecting
    try:
        c, _ = login("Sam", host, port, uid=uuid.uuid3(uuid.NAMESPACE_DNS, "Sam"))
        break
    except AssertionError:
        time.sleep(0.2)

# 1. /effect give: update_mob_effect (blend flag on first add) + the speed attribute modifier (0.2F * 2).
cmd(c, "effect clear @s")  # a rerun on the same world: effects persist in the player file
cmd(c, "effect give @s minecraft:speed 30 1")
while (e := effect_added(c, SPEED))[1] != 600: pass
assert e == (1, 600, 2 | 4 | 8), e
want = struct.unpack(">f", struct.pack(">f", 0.2))[0] * 2  # 0.2F widened to double, times amplifier + 1
for _ in range(5):
    if (m := speed_modifier(c)) is not None: break
assert m == want, m
cmd(c, "effect clear @s minecraft:speed")
removed(c, SPEED)
assert speed_modifier(c) is None
c.eid = getattr(c, "eid", 0)
print("effect give/clear + speed attribute ok")

# 2. Golden apple: regeneration II (100 ticks) + absorption (2400); milk clears them.
cmd(c, "gamemode survival")
cmd(c, "clear @s")
cmd(c, "give @s golden_apple")
wait(c, PLAY_CB["container_set_slot"])
c.send(PLAY_SB["use_item"], varint(0) + varint(1) + struct.pack(">ff", 0, 0))
assert effect_added(c, REGEN)[:2] == (1, 100)
assert effect_added(c, ABSORB)[:2] == (0, 2400)
cmd(c, "clear @s")
cmd(c, "give @s milk_bucket")
wait(c, PLAY_CB["container_set_slot"])
c.send(PLAY_SB["use_item"], varint(0) + varint(2) + struct.pack(">ff", 0, 0))
removed(c, REGEN)
print("golden apple + milk ok")

# 3. Resistance I: 6 damage * 20/25 = 4.8 (no armour) -> health 15.2.
cmd(c, "effect give @s minecraft:resistance 30 0")
effect_added(c, RESIST)
cmd(c, "damage @s 6 minecraft:generic")
while abs((h := health(c)) - 15.2) > 1e-4 and h == 20.0: pass
assert abs(h - 15.2) < 1e-4, h
print("resistance ok")

# 4. XP: levels saturate at 0 and reset progress/total (giveExperienceLevels); diamond ore drops 3-7 xp in orbs.
def experience(c):
    b = wait(c, PLAY_CB["set_experience"])
    return struct.unpack(">f", b.take(4))[0], b.varint(), b.varint()
cmd(c, "experience add @s -100 levels")  # level 0, progress 0, total 0
cmd(c, "experience add @s 20 points")
while experience(c)[1:] != (2, 20): pass  # 7 to level 1, 9 to level 2, 4/11 into level 2
cmd(c, "experience add @s -5 levels")
while experience(c) != (0.0, 0, 0): pass
ORB = ids("entity_types")["experience_orb"]
cmd(c, "clear @s")
cmd(c, "give @s iron_pickaxe")
wait(c, PLAY_CB["container_set_slot"])
cmd(c, "setblock 3 -61 3 minecraft:diamond_ore")
walk(c, 3.5, -60, 2.5, True)
time.sleep(0.3)
c.send(PLAY_SB["player_action"], varint(0) + struct.pack(">q", (3 & 0x3FFFFFF) << 38 | (3 & 0x3FFFFFF) << 12 | (-61 & 0xFFF)) + b"\x01" + varint(1))
time.sleep(1.2)
c.send(PLAY_SB["player_action"], varint(3) + struct.pack(">q", (3 & 0x3FFFFFF) << 38 | (3 & 0x3FFFFFF) << 12 | (-61 & 0xFFF)) + b"\x01" + varint(2))
wait(c, PLAY_CB["add_entity"], lambda b: (b.varint(), b.take(16), b.varint())[2] == ORB)
print("experience ok")
cmd(c, "gamemode creative")
c.close() if hasattr(c, "close") else None
