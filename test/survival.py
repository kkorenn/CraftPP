#!/usr/bin/env python3
"""Survival flows: break a block -> item entity -> pickup; craft planks via container_click; fall damage.
Usage: python3 test/survival.py host port  (flat test world)"""
import json, os, struct, sys, time, uuid
sys.path.insert(0, os.path.dirname(__file__))
from bot import *
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ITEMS = json.load(open(os.path.join(ROOT, "vanilla/generated/reports/registries.json")))["minecraft:item"]["entries"]
ENTITY = ids("entity_types")
def pos(x, y, z): return struct.pack(">q", (x & 0x3FFFFFF) << 38 | (z & 0x3FFFFFF) << 12 | (y & 0xFFF))

def item(n): return ITEMS["minecraft:" + n]["protocol_id"]
DIRT, LOG, PLANKS = item("dirt"), item("oak_log"), item("oak_planks")

def slot_item(b):  # ItemStack.OPTIONAL_STREAM_CODEC -> (count, item)
    n = b.varint()
    return (n, b.varint()) if n > 0 else (0, -1)

def move(c, x, y, z, ground):
    c.send(PLAY_SB["move_player_pos"], struct.pack(">dddB", x, y, z, 1 if ground else 0))

def click(c, slot, button, kind):  # container 0, no predicted changes
    c.send(PLAY_SB["container_click"], varint(0) + varint(getattr(c, "state", 0)) + struct.pack(">hb", slot, button) + varint(kind) + varint(0) + b"\0")

def skip_patch(b):
    for _ in range(b.varint()): raise AssertionError("components not expected in test")
    b.varint()

def wait_slot(c, slot, want):
    """Wait until the server shows `want` in menu `slot` (set_slot, or a full set_content resync)."""
    for _ in range(2000):
        p, b = c.recv()
        if p == PLAY_CB["container_set_slot"]:
            b.varint(); c.state = b.varint()
            if b.i16() == slot and slot_item(b) == want: return
        elif p == PLAY_CB["container_set_content"]:
            b.varint(); c.state = b.varint()
            items = []
            for _ in range(b.varint()):
                it = slot_item(b)
                if it[0]: skip_patch(b)
                items.append(it)
            if items[slot] == want: return
    raise AssertionError(f"slot {slot} never became {want}")

host, port = sys.argv[1], int(sys.argv[2])
c, _ = login("Sam", host, port, uid=uuid.uuid3(uuid.NAMESPACE_DNS, "Sam"))
c.send(PLAY_SB["chat_command"], string("gamemode survival"))
wait(c, PLAY_CB["game_event"], lambda b: b.d[b.i] == 3)
move(c, 1.5, -60, 1.5, True)

# 1. Break grass by hand (0.6 hardness -> ~13+ ticks), the dirt it drops falls into the hole; stand in it.
t0 = time.time()
c.send(PLAY_SB["player_action"], varint(0) + pos(1, -61, 1) + b"\x01" + varint(1))
time.sleep(1.0)
c.send(PLAY_SB["player_action"], varint(3) + pos(1, -61, 1) + b"\x01" + varint(2))
b = wait(c, PLAY_CB["add_entity"], lambda b: (b.varint(), b.take(16), b.varint())[2] == ENTITY["item"])
data = wait(c, PLAY_CB["set_entity_data"])
data.varint(); assert data.u8() == 8 and data.varint() == 7
assert slot_item(data) == (1, DIRT), "grass should drop one dirt"
print("break -> item entity ok")
move(c, 1.5, -61, 1.5, True)
wait(c, PLAY_CB["take_item_entity"])
wait_slot(c, 36, (1, DIRT))
print("pickup ok (%.1fs)" % (time.time() - t0))

# 2. Craft: log into the 2x2 grid -> 4 planks preview -> shift-click result into the inventory.
c.send(PLAY_SB["chat_command"], string("gamemode creative"))
wait(c, PLAY_CB["game_event"], lambda b: b.d[b.i] == 3)
c.send(PLAY_SB["set_creative_mode_slot"], struct.pack(">h", 37) + varint(1) + varint(LOG) + varint(0) + varint(0))
click(c, 37, 0, 0)   # pick up the log
click(c, 1, 0, 0)    # put it in the grid
wait_slot(c, 0, (4, PLANKS))
click(c, 0, 0, 1)    # quick move the result
wait_slot(c, 44, (4, PLANKS))
print("crafting ok")

# 3. Fall 10 blocks in survival: floor(10 - 3) = 7 damage -> 13 health.
c.send(PLAY_SB["chat_command"], string("gamemode survival"))
wait(c, PLAY_CB["game_event"], lambda b: b.d[b.i] == 3)
move(c, 4.5, -50, 4.5, False)
move(c, 4.5, -60, 4.5, True)
h = wait(c, PLAY_CB["set_health"], lambda b: struct.unpack(">f", b.d[b.i:b.i + 4])[0] < 20)
assert struct.unpack(">f", h.take(4))[0] == 13.0
print("fall damage ok")
