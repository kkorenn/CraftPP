#!/usr/bin/env python3
"""Chest + blast furnace round trip and persistence.
  python3 test/containers_test.py host port                 -> place chest/furnace, fill, smelt
  python3 test/containers_test.py host port --after-restart -> contents survived the restart"""
import faulthandler, json, os, struct, sys, time, uuid
faulthandler.dump_traceback_later(60, exit=True)
sys.path.insert(0, os.path.dirname(__file__))
from bot import *

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = json.load(open(os.path.join(ROOT, "data/game.json")))  # ids without needing vanilla/
ITEMS = {it["n"]: i for i, it in enumerate(GAME["items"])}
MENU = {"minecraft:" + k: {"protocol_id": v} for k, v in GAME["registries"]["menu"].items()}
def item(n): return ITEMS[n]
def pos(x, y, z): return struct.pack(">q", (x & 0x3FFFFFF) << 38 | (z & 0x3FFFFFF) << 12 | (y & 0xFFF))
UID = uuid.uuid3(uuid.NAMESPACE_DNS, "Cora")
CHEST, FURNACE = (20, -60, 20), (22, -60, 20)

def slot_item(b):
    n = b.varint()
    if n <= 0: return (0, -1)
    it = b.varint()
    assert b.varint() == 0 and b.varint() == 0, "no components expected"
    return (n, it)

def give(c, hotbar, name, count):
    c.send(PLAY_SB["set_creative_mode_slot"], struct.pack(">h", 36 + hotbar) + varint(count) + varint(item(name)) + varint(0) + varint(0))

def use(c, p, seq):
    c.send(PLAY_SB["use_item_on"], varint(0) + pos(*p) + varint(1) + struct.pack(">fff", .5, 1, .5) + b"\0\0" + varint(seq))

def open_menu(c, p, seq, menu):
    use(c, p, seq)
    o = wait(c, PLAY_CB["open_screen"])
    cid = o.varint()
    assert o.varint() == MENU["minecraft:" + menu]["protocol_id"], "wrong menu type"
    b = wait(c, PLAY_CB["container_set_content"], lambda b: b.varint() == cid)
    c.state = b.varint()
    items = [slot_item(b) for _ in range(b.varint())]
    return cid, items

def click(c, cid, slot, button, kind):
    c.send(PLAY_SB["container_click"], varint(cid) + varint(c.state) + struct.pack(">hb", slot, button) + varint(kind) + varint(0) + b"\0")

def wait_slot(c, cid, slot, want, limit=4000):
    for _ in range(limit):
        p, b = c.recv()
        if p == PLAY_CB["container_set_slot"] and b.varint() == cid:
            c.state = b.varint()
            if b.i16() == slot and slot_item(b) == want: return
        elif p == PLAY_CB["container_set_content"] and b.varint() == cid:
            c.state = b.varint()
            if [slot_item(b) for _ in range(b.varint())][slot] == want: return
    raise AssertionError(f"slot {slot} never became {want}")

def close(c, cid):
    c.send(PLAY_SB["container_close"], varint(cid))

host, port = sys.argv[1], int(sys.argv[2])
c, _ = login("Cora", host, port, uid=UID)
c.send(PLAY_SB["move_player_pos"], struct.pack(">dddB", 21.5, -60, 22.5, 1))

if "--after-restart" in sys.argv:
    cid, items = open_menu(c, CHEST, 1, "generic_9x3")
    assert items[0] == (40, item("cobblestone")), items[:3]
    close(c, cid)
    cid, items = open_menu(c, FURNACE, 2, "blast_furnace")
    assert items[2][1] == item("iron_ingot") and items[2][0] >= 1, items[:3]
    print("containers persist ok")
    sys.exit()

# Chest: place, open, move 40 cobblestone from the hotbar into slot 0, reopen and check.
give(c, 0, "chest", 1)
use(c, (CHEST[0], CHEST[1] - 1, CHEST[2]), 1)
wait(c, PLAY_CB["block_update"], lambda b: block_pos(b.i64()) == CHEST)
give(c, 0, "cobblestone", 40)
cid, items = open_menu(c, CHEST, 2, "generic_9x3")
assert len(items) == 63 and items[54] == (40, item("cobblestone")), items[54]
click(c, cid, 54, 0, 0)  # pick up the hotbar stack
click(c, cid, 0, 0, 0)   # into chest slot 0
wait_slot(c, cid, 0, (40, item("cobblestone")))
close(c, cid)
cid, items = open_menu(c, CHEST, 3, "generic_9x3")
assert items[0] == (40, item("cobblestone")) and items[54] == (0, -1), items[:1] + items[54:55]
close(c, cid)
print("chest ok")

# Blast furnace: raw iron + coal by shift-click, iron ingot after 100 ticks (200 / speed 2).
give(c, 0, "blast_furnace", 1)
use(c, (FURNACE[0], FURNACE[1] - 1, FURNACE[2]), 4)
wait(c, PLAY_CB["block_update"], lambda b: block_pos(b.i64()) == FURNACE)
give(c, 1, "coal", 1)
give(c, 2, "raw_iron", 2)
cid, items = open_menu(c, FURNACE, 5, "blast_furnace")
assert len(items) == 39
click(c, cid, 32, 0, 1)  # raw iron -> input
click(c, cid, 31, 0, 1)  # coal -> fuel
t0, lit_total, done = time.time(), None, False
while not done:  # until the ingot shows up in the result slot; record the furnace's lit_total data slot
    p, b = c.recv()
    if p == PLAY_CB["container_set_data"] and b.varint() == cid and b.i16() == 1: lit_total = b.i16() or lit_total
    elif p == PLAY_CB["container_set_slot"] and b.varint() == cid:
        c.state = b.varint()
        done = b.i16() == 2 and slot_item(b) == (1, item("iron_ingot"))
assert lit_total == 800, f"coal burns 1600/2 ticks in a blast furnace, got {lit_total}"
assert time.time() - t0 > 4, "smelting takes 100 ticks"
print("blast furnace ok (%.1fs)" % (time.time() - t0))
close(c, cid)
