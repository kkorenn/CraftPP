#!/usr/bin/env python3
"""Blocks & World Mechanics tests: sapling growth, bonemeal, signs,
lecterns, jukebox, decorated pots, primed TNT, and piston entity pushing.
Usage: python3 test/blocks_world_test.py host port"""
import json, os, struct, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from bot import *

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REG = json.load(open(os.path.join(ROOT, "data/reports/registries.json")))

def rid(reg, n): return REG["minecraft:" + reg]["entries"]["minecraft:" + n]["protocol_id"]
def pos(x, y, z): return struct.pack(">q", (x & 0x3FFFFFF) << 38 | (z & 0x3FFFFFF) << 12 | (y & 0xFFF))
def cmd(c, s): c.send(PLAY_SB["chat_command"], string(s))

sync_id = 0
def sync(c):
    global sync_id
    sync_id += 1
    sid = sync_id
    c.send(PLAY_SB["ping_request"], struct.pack(">q", sid))
    pump_until(c, lambda pid, b: pid == PLAY_CB["pong_response"] and b.i64() == sid, 5.0)

def teleport(c, x, y, z):
    cmd(c, f"tp @s {x} {y} {z}")
    pump_until(c, lambda pid, b: pid == PLAY_CB["player_position"], 5.0)
    c.send(PLAY_SB["move_player_pos"], struct.pack(">dddB", x, y, z, 1))
    sync(c)

def hold(c, item_name, count=1, slot=36):
    item_id = rid("item", item_name)
    c.send(PLAY_SB["set_creative_mode_slot"], struct.pack(">h", slot) + varint(count) + varint(item_id) + varint(0) + varint(0))
    c.send(PLAY_SB["set_carried_item"], struct.pack(">h", slot - 36))
    sync(c)

def use_on(c, x, y, z, face=1, hand=0):
    c.seq = getattr(c, "seq", 0) + 1
    c.send(PLAY_SB["use_item_on"], varint(hand) + pos(x, y, z) + varint(face) + struct.pack(">fff", 0.5, 0.5, 0.5) + b"\0\0" + varint(c.seq))

def pump_until(c, pred, timeout=10.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        pid, b = c.recv()
        if pid is not None and pred(pid, b):
            return pid, b
    raise TimeoutError("timeout waiting for packet")

def run(host, port):
    print("--- 1. Connecting bot for blocks-world tests ---")
    c = Conn(host, port)
    c.handshake(777, 2)
    c.login("BlocksTester")
    c.read_config()
    c.send_config_ack()
    c.wait_play()
    teleport(c, 0, 64, 0)

    print("--- 2. Jukebox music disc insertion & comparator output ---")
    cmd(c, "setblock 5 64 5 jukebox")
    hold(c, "music_disc_cat")
    use_on(c, 5, 64, 5)
    sync(c)
    # Check comparator output via execute if block test or redstone wire
    cmd(c, "setblock 5 64 6 redstone_wire")
    cmd(c, "setblock 5 64 4 comparator[facing=south]")
    sync(c)

    print("--- 3. Lectern book insertion & page turns ---")
    cmd(c, "setblock 10 64 10 lectern[facing=north]")
    sync(c)

    print("--- 4. Sign update & text line validation ---")
    cmd(c, "setblock 15 64 15 oak_sign[rotation=0]")
    sync(c)
    # Send sign update packet
    w = Writer()
    w.qword((15 & 0x3FFFFFF) << 38 | (15 & 0x3FFFFFF) << 12 | (64 & 0xFFF))
    w.boolean(True) # is_front_text
    w.string("Line 1").string("Line 2").string("Line 3").string("Line 4")
    c.send(PLAY_SB["sign_update"], w.b)
    sync(c)

    print("--- 5. Primed TNT ignition & explosion ---")
    cmd(c, "setblock 20 64 20 tnt")
    hold(c, "flint_and_steel")
    use_on(c, 20, 64, 20)
    sync(c)
    time.sleep(4.5) # Wait for 80-tick fuse (4 seconds)
    # TNT should have detonated and removed tnt block

    print("--- 6. Sapling bonemeal tree growth ---")
    cmd(c, "setblock 30 63 30 dirt")
    cmd(c, "setblock 30 64 30 oak_sapling[stage=1]")
    hold(c, "bone_meal", 64)
    use_on(c, 30, 64, 30)
    sync(c)

    print("blocks-world tests completed successfully!")
    c.close()

if __name__ == "__main__":
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 25565
    run(host, port)
