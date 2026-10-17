#!/usr/bin/env python3
"""Block placement states (BlockItem.place + getStateForPlacement) and neighbour shape updates: logs, stairs, slabs,
furnaces, signs, banners, skulls, candles, walls, fences, chests, rails, redstone wire, observers, hoppers.
Usage: python3 test/placement_test.py host port  (flat creative test world, Alice is op)"""
import json, os, struct, sys, time, uuid
sys.path.insert(0, os.path.dirname(__file__))
from bot import *
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORT = json.load(open(os.path.join(ROOT, "vanilla/generated/reports/blocks.json")))
ITEMS = json.load(open(os.path.join(ROOT, "vanilla/generated/reports/registries.json")))["minecraft:item"]["entries"]

def state(name, **props):
    for s in REPORT["minecraft:" + name]["states"]:
        sp = s.get("properties", {})
        if all(sp.get(k) == str(v).lower() for k, v in props.items()) and len(props) == len(sp):
            return s["id"]
    raise KeyError(name, props)
def pos(x, y, z): return struct.pack(">q", (x & 0x3FFFFFF) << 38 | (z & 0x3FFFFFF) << 12 | (y & 0xFFF))

host, port = sys.argv[1], int(sys.argv[2])
c, _ = login("Alice", host, port, decode=True, uid=uuid.uuid3(uuid.NAMESPACE_DNS, "Alice"))
c.s.settimeout(10)

def at(x, y, z):
    secs = c.chunks.get((x >> 4, z >> 4))
    return secs[(y + 64) >> 4][0][((y & 15) * 16 + (z & 15)) * 16 + (x & 15)] if secs else None

def pump(seconds):
    end = time.time() + seconds
    while time.time() < end:
        pid, b = c.recv()
        if pid == PLAY_CB["level_chunk_with_light"]:
            cx, cz, secs = parse_chunk(b, True)[:3]; c.chunks[cx, cz] = secs
        elif pid == PLAY_CB["block_update"]:
            set_block(c, *block_pos(b.i64()), b.varint())
        elif pid == PLAY_CB["section_blocks_update"]:
            v = b.i64(); sx, sy, sz = v >> 42, sext(v, 20), sext(v >> 20, 22)
            for _ in range(b.varint()):
                e = b.varint()
                set_block(c, sx * 16 + (e >> 8 & 15), sy * 16 + (e & 15), sz * 16 + (e >> 4 & 15), e >> 12)

def cmd(s): c.send(PLAY_SB["chat_command"], string(s))

def wait_until(pred, seconds, what):
    end = time.time() + seconds
    while time.time() < end:
        if pred(): return
        pump(0.05)
    raise AssertionError(what)

Y, Z0 = -60, 500  # first air layer of the flat world, away from the other tests
cmd("gamemode creative")
pump(0.5)

def look(yaw, pitch):
    c.send(PLAY_SB["move_player_rot"], struct.pack(">ffB", yaw, pitch, 1))

def place(item, x, y, z, face, cx=.5, cy=.5, cz=.5, yaw=0.0, pitch=30.0):
    if abs(place.at[0] - x) > 2 or abs(place.at[1] - z) > 3:  # stay within reach, out of the way (z - 3)
        place.at = (x, z)
        cmd(f"tp Alice {x + 0.5} {Y} {z - 2.5}")
        wait_until(lambda: getattr(c, "pos", None) == (x + 0.5, Y, z - 2.5), 3, "teleport")
        pump(0.2)
    look(yaw, pitch)
    c.send(PLAY_SB["set_creative_mode_slot"], struct.pack(">h", 36) + varint(1) + varint(ITEMS["minecraft:" + item]["protocol_id"]) + varint(0) + varint(0))
    place.seq += 1
    c.send(PLAY_SB["use_item_on"], varint(0) + pos(x, y, z) + varint(face) + struct.pack(">fff", cx, cy, cz) + b"\0\0" + varint(place.seq))
    pump(0.15)
place.seq = 100
place.at = (0, 0)
DOWN, UP, NORTH, SOUTH, WEST, EAST = range(6)

def expect(x, y, z, name, **props):
    want = state(name, **props)
    wait_until(lambda: at(x, y, z) == want, 2, f"{name} {props} at {x},{y},{z}: got {at(x, y, z)} want {want}, below {at(x, y - 1, z)}, pos {getattr(c, 'pos', None)}")

def setb(x, y, z, b):
    cmd(f"setblock {x} {y} {z} {b}")

X = 100
cmd(f"tp Alice {X + 0.5} {Y} {Z0 - 2.5}")
pump(2.0)
place.at = (X, Z0)
# Clicking the ground's top face places at Y; clicking a block's side places next to it.
place("oak_log", X, Y - 1, Z0, UP)
expect(X, Y, Z0, "oak_log", axis="y")
place("oak_log", X, Y, Z0, EAST)
expect(X + 1, Y, Z0, "oak_log", axis="x")
# Stairs face the player's horizontal direction; the upper half of a side click flips them.
place("oak_stairs", X + 3, Y - 1, Z0, UP, yaw=180)  # looking north
expect(X + 3, Y, Z0, "oak_stairs", facing="north", half="bottom", shape="straight", waterlogged=False)
place("oak_stairs", X + 3, Y, Z0, EAST, cy=.8, yaw=180)
expect(X + 4, Y, Z0, "oak_stairs", facing="north", half="top", shape="straight", waterlogged=False)
# Slab merging into a double slab.
place("oak_slab", X + 6, Y - 1, Z0, UP)
expect(X + 6, Y, Z0, "oak_slab", type="bottom", waterlogged=False)
place("oak_slab", X + 6, Y, Z0, UP, cy=.5)
expect(X + 6, Y, Z0, "oak_slab", type="double", waterlogged=False)
# Furnaces face the player; observers look where the player looks; hoppers point into the clicked block.
place("furnace", X + 8, Y - 1, Z0, UP, yaw=0)  # looking south
expect(X + 8, Y, Z0, "furnace", facing="north", lit=False)
place("observer", X + 9, Y - 1, Z0, UP, pitch=90)
expect(X + 9, Y, Z0, "observer", facing="down", powered=False)
place("hopper", X + 9, Y, Z0, EAST)
expect(X + 10, Y, Z0, "hopper", facing="west", enabled=True)

Z = Z0 + 4
# Standing signs, banners and skulls take one of 16 rotations; clicking a wall gives the wall variant.
place("oak_sign", X, Y - 1, Z, UP, yaw=0)
expect(X, Y, Z, "oak_sign", rotation=8, waterlogged=False)
place("white_banner", X + 1, Y - 1, Z, UP, yaw=90)
expect(X + 1, Y, Z, "white_banner", rotation=12)
place("skeleton_skull", X + 2, Y - 1, Z, UP, yaw=45)
expect(X + 2, Y, Z, "skeleton_skull", rotation=2, powered=False)
setb(X + 4, Y, Z, "stone")
place("oak_sign", X + 4, Y, Z, EAST, yaw=90)
expect(X + 5, Y, Z, "oak_wall_sign", facing="east", waterlogged=False)
place("skeleton_skull", X + 4, Y, Z, WEST, yaw=-90)
expect(X + 3, Y, Z, "skeleton_wall_skull", facing="west", powered=False)
# Candles and sea pickles stack when placed into themselves.
place("candle", X + 7, Y - 1, Z, UP)
expect(X + 7, Y, Z, "candle", candles=1, lit=False, waterlogged=False)
place("candle", X + 7, Y, Z, UP)
expect(X + 7, Y, Z, "candle", candles=2, lit=False, waterlogged=False)

Z = Z0 + 8
# Walls and fences connect on placement and disconnect when the neighbour goes.
place("cobblestone_wall", X, Y - 1, Z, UP)
expect(X, Y, Z, "cobblestone_wall", up=True, north="none", south="none", east="none", west="none", waterlogged=False)
place("cobblestone_wall", X, Y, Z, EAST)
expect(X + 1, Y, Z, "cobblestone_wall", up=True, west="low", north="none", south="none", east="none", waterlogged=False)
expect(X, Y, Z, "cobblestone_wall", up=True, east="low", north="none", south="none", west="none", waterlogged=False)
place("cobblestone_wall", X + 1, Y, Z, EAST)  # straight run of three: the middle loses its post
expect(X + 1, Y, Z, "cobblestone_wall", up=False, west="low", east="low", north="none", south="none", waterlogged=False)
place("oak_fence", X + 5, Y - 1, Z, UP)
place("oak_fence", X + 5, Y, Z, SOUTH)
expect(X + 5, Y, Z, "oak_fence", south=True, north=False, east=False, west=False, waterlogged=False)
setb(X + 5, Y, Z + 1, "air")
expect(X + 5, Y, Z, "oak_fence", south=False, north=False, east=False, west=False, waterlogged=False)
# Two chests side by side with the same facing become a double chest.
place("chest", X + 8, Y - 1, Z, UP, yaw=0)
expect(X + 8, Y, Z, "chest", facing="north", type="single", waterlogged=False)
place("chest", X + 9, Y - 1, Z, UP, yaw=0)
expect(X + 9, Y, Z, "chest", facing="north", type="right", waterlogged=False)
expect(X + 8, Y, Z, "chest", facing="north", type="left", waterlogged=False)

Z = Z0 + 12
# Rails curve toward neighbouring rails (RailState); a lone redstone wire is a cross.
place("rail", X, Y - 1, Z, UP, yaw=0)
place("rail", X + 1, Y - 1, Z, UP, yaw=0)
place("rail", X, Y - 1, Z + 1, UP, yaw=0)
expect(X, Y, Z, "rail", shape="south_east", waterlogged=False)
place("redstone", X + 4, Y - 1, Z, UP)
expect(X + 4, Y, Z, "redstone_wire", north="side", south="side", east="side", west="side", power=0)
place("redstone", X + 5, Y - 1, Z, UP)  # a neighbour turns the cross into a line
expect(X + 4, Y, Z, "redstone_wire", north="none", south="none", east="side", west="side", power=0)
# End rods point out of the clicked face; banners on a wall become wall banners.
place("end_rod", X + 7, Y - 1, Z, UP)
expect(X + 7, Y, Z, "end_rod", facing="up")
setb(X + 9, Y, Z, "stone")
place("white_banner", X + 9, Y, Z, NORTH, yaw=0)
expect(X + 9, Y, Z - 1, "white_wall_banner", facing="north")
c.s.close()
print("placement test passed")
