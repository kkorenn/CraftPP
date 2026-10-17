#!/usr/bin/env python3
"""Advancements, recipe book and statistics (/advancement, /recipe, inventory_changed, award_stats)."""
import json, os, sys, time, uuid
sys.path.insert(0, os.path.dirname(__file__))
from bot import *
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REG = json.load(open(os.path.join(ROOT, "data/reports/registries.json")))
def rid(reg, n): return REG["minecraft:" + reg]["entries"]["minecraft:" + n]["protocol_id"]
def cmd(c, s): c.send(PLAY_SB["chat_command"], string(s))

host, port = sys.argv[1], int(sys.argv[2])
for attempt in range(50):
    try:
        c, _ = login("Sam", host, port, uid=uuid.uuid3(uuid.NAMESPACE_DNS, "Sam"))
        break
    except AssertionError:
        time.sleep(0.2)

# 1. /advancement revoke + grant -> update_advancements packet.
cmd(c, "advancement revoke @s everything")
cmd(c, "advancement grant @s only minecraft:story/mine_stone")
wait(c, PLAY_CB["update_advancements"], lambda b: b"minecraft:story/mine_stone" in b.b[b.i:], 5000)

# 2. inventory_changed trigger: revoke story/root, give crafting_table -> story/root awarded.
cmd(c, "clear @s")
cmd(c, "advancement revoke @s everything")
cmd(c, "give @s minecraft:crafting_table")
wait(c, PLAY_CB["update_advancements"], lambda b: b"minecraft:story/root" in b.b[b.i:], 5000)

# 3. /recipe take + give -> recipe_book_add + recipe_book_remove packets.
cmd(c, "recipe take @s *")
cmd(c, "recipe give @s minecraft:oak_planks")
b = wait(c, PLAY_CB["recipe_book_add"], lambda b: b.varint() >= 1, 5000)
cmd(c, "recipe take @s minecraft:oak_planks")
wait(c, PLAY_CB["recipe_book_remove"], lambda b: b.varint() >= 1, 5000)

# 4. Statistics: request_stats (client_command action 1) -> award_stats with play_time > 0.
CUSTOM_TYPE = rid("stat_type", "custom")
PLAY_TIME = rid("custom_stat", "play_time")
c.send(PLAY_SB["client_command"], varint(1))
b = wait(c, PLAY_CB["award_stats"], limit=5000)
stats = {}
for _ in range(b.varint()):
    stats[(b.varint(), b.varint())] = b.varint()
assert stats.get((CUSTOM_TYPE, PLAY_TIME), 0) > 0, stats

c.s.close()
print("progression: advancements, recipe book, stats PASS")
