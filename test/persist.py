#!/usr/bin/env python3
"""Block place/break + persistence check.
  python3 test/persist.py host port                 -> place/break, move, rejoin, verify
  python3 test/persist.py host port --after-restart -> verify the same state survived a server restart"""
import json, os, struct, sys, time, uuid
sys.path.insert(0, os.path.dirname(__file__))
from bot import *

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ITEMS = json.load(open(os.path.join(ROOT, "vanilla/generated/reports/registries.json")))["minecraft:item"]["entries"]
STONE_ITEM = ITEMS["minecraft:stone"]["protocol_id"]
STONE, AIR = int(re.search(r"int stone = (\d+);", HDR).group(1)), 0
UID = uuid.uuid3(uuid.NAMESPACE_DNS, "Steve")

def pos(x, y, z): return struct.pack(">q", (x & 0x3FFFFFF) << 38 | (z & 0x3FFFFFF) << 12 | (y & 0xFFF))
def block_at(c, x, y, z): return c.chunks[x >> 4, z >> 4][(y + 64) >> 4][0][((y & 15) * 16 + (z & 15)) * 16 + (x & 15)]

def verify(c):
    assert abs(c.pos[0] - 5.5) < 1e-9 and abs(c.pos[2] - 7.5) < 1e-9, f"position not restored: {c.pos}"
    assert block_at(c, 0, -60, 0) == STONE, "placed block lost"
    assert block_at(c, 2, -61, 2) == AIR, "broken block came back"

host, port = sys.argv[1], int(sys.argv[2])
if "--after-restart" in sys.argv:
    c, _ = login("Steve", host, port, decode=True, uid=UID)
    verify(c)
    print("persist ok: survived restart")
    sys.exit()

c, _ = login("Steve", host, port, decode=True, uid=UID)
c.send(PLAY_SB["set_creative_mode_slot"], struct.pack(">h", 36) + varint(1) + varint(STONE_ITEM) + varint(0) + varint(0))
c.send(PLAY_SB["move_player_pos"], struct.pack(">dddB", 2.5, -60, 0.5, 1))  # out of the way: placement checks the player box
c.send(PLAY_SB["use_item_on"], varint(0) + pos(0, -61, 0) + varint(1) + struct.pack(">fff", .5, 1, .5) + b"\0\0" + varint(1))
wait(c, PLAY_CB["block_update"], lambda b: (block_pos(b.i64()), b.varint()) == ((0, -60, 0), STONE), limit=100000)  # mob movement packets interleave
c.send(PLAY_SB["player_action"], varint(0) + pos(2, -61, 2) + b"\x01" + varint(2))
wait(c, PLAY_CB["block_update"], lambda b: (block_pos(b.i64()), b.varint()) == ((2, -61, 2), AIR), limit=100000)
# Middle-click bedrock: slot 0 holds stone, so bedrock goes to hotbar slot 1 and it gets selected.
c.send(PLAY_SB["pick_item_from_block"], pos(0, -64, 0) + b"\0")
inv = wait(c, PLAY_CB["set_player_inventory"])
got = (inv.varint(), inv.varint(), inv.varint())
assert got == (1, 1, ITEMS["minecraft:bedrock"]["protocol_id"]), (got, ITEMS["minecraft:bedrock"]["protocol_id"])
assert wait(c, PLAY_CB["set_held_slot"]).varint() == 1
print("pick block ok")
c.send(PLAY_SB["move_player_pos"], struct.pack(">dddB", 5.5, -60, 7.5, 1))
time.sleep(0.3)
c.s.shutdown(socket.SHUT_RDWR)
time.sleep(0.3)

c, _ = login("Steve", host, port, decode=True, uid=UID)
verify(c)
print("persist ok: place/break/position kept across rejoin")
