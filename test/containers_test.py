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
x0, _, z0 = c.pos
for k in range(1, 9):  # walk over in small steps (the server rejects moves that are too fast)
    c.send(PLAY_SB["move_player_pos"], struct.pack(">dddB", x0 + (21.5 - x0) * k / 8, -60, z0 + (22.5 - z0) * k / 8, 1))
    time.sleep(0.06)

if "--after-restart" in sys.argv:
    cid, items = open_menu(c, CHEST, 1, "generic_9x3")
    assert items[0] == (45, item("cobblestone")), items[:3]
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

# Hopper: sneak-place against the chest's east face -> hopper at x+1 facing west; 5 cobblestone flow into the chest.
give(c, 0, "hopper", 1)
c.send(PLAY_SB["player_input"], b"\x20")
c.send(PLAY_SB["use_item_on"], varint(0) + pos(*CHEST) + varint(5) + struct.pack(">fff", 1, .5, .5) + b"\0\0" + varint(6))
wait(c, PLAY_CB["block_update"], lambda b: block_pos(b.i64()) == (CHEST[0] + 1, CHEST[1], CHEST[2]))
c.send(PLAY_SB["player_input"], b"\x00")
give(c, 0, "cobblestone", 5)
time.sleep(0.2)
cid, items = open_menu(c, (CHEST[0] + 1, CHEST[1], CHEST[2]), 7, "hopper")
assert len(items) == 41
click(c, cid, 32, 0, 1)  # hotbar 0 -> hopper
time.sleep(4)            # 5 items x 8 ticks
close(c, cid)
cid, items = open_menu(c, CHEST, 8, "generic_9x3")
assert items[0] == (45, item("cobblestone")), items[:2]
close(c, cid)
print("hopper ok")

# Stonecutter: 2 stone, pick the stone_slab option -> 2 slabs in the result slot (client list = update_recipes order).
CDATA = json.load(open(os.path.join(ROOT, "data/containers.json")))
opts = [r["r"][0] for r in CDATA["stonecutting"] if item("stone") in r["i"]]
STONECUTTER = (24, -60, 22)
give(c, 0, "stonecutter", 1)
use(c, (STONECUTTER[0], STONECUTTER[1] - 1, STONECUTTER[2]), 9)
wait(c, PLAY_CB["block_update"], lambda b: block_pos(b.i64()) == STONECUTTER)
give(c, 0, "stone", 2)
time.sleep(0.2)
cid, items = open_menu(c, STONECUTTER, 10, "stonecutter")
assert len(items) == 38
click(c, cid, 29, 0, 1)  # stone -> input
wait_slot(c, cid, 0, (2, item("stone")))
c.send(PLAY_SB["container_button_click"], varint(cid) + varint(opts.index(item("stone_slab"))))
wait_slot(c, cid, 1, (2, item("stone_slab")))
close(c, cid)
print("stonecutter ok")

# Anvil rename: the result carries minecraft:custom_name (component patch present).
ANVIL = (26, -60, 22)
give(c, 0, "anvil", 1)
use(c, (ANVIL[0], ANVIL[1] - 1, ANVIL[2]), 11)
wait(c, PLAY_CB["block_update"], lambda b: block_pos(b.i64()) == ANVIL)
give(c, 0, "iron_sword", 1)
time.sleep(0.2)
cid, items = open_menu(c, ANVIL, 12, "anvil")
click(c, cid, 30, 0, 1)  # sword -> input
wait_slot(c, cid, 0, (1, item("iron_sword")))
c.send(PLAY_SB["rename_item"], string("Sting"))
def named(b):
    if b.varint() != cid: return False
    b.varint()
    if b.i16() != 2 or b.varint() != 1 or b.varint() != item("iron_sword"): return False
    return b"Sting" in b.d[b.i:]
wait(c, PLAY_CB["container_set_slot"], named)
close(c, cid)
print("anvil rename ok")
