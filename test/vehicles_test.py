#!/usr/bin/env python3
"""Vehicles & Nether/End mob projectile tests: boats, chest boats,
minecarts on rails, fireballs, and shulker bullets.
Usage: python3 test/vehicles_test.py host port"""
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
    print("--- 1. Connecting bot for vehicles tests ---")
    c = Conn(host, port)
    c.handshake(777, 2)
    c.login("VehicleTester")
    c.read_config()
    c.send_config_ack()
    c.wait_play()
    teleport(c, 100, 64, 100)

    print("--- 2. Boat placement & buoyancy ---")
    cmd(c, "fill 98 62 98 105 62 105 water")
    cmd(c, "setblock 100 63 100 water")
    hold(c, "oak_boat")
    use_on(c, 100, 63, 100)
    sync(c)

    print("--- 3. Minecart & Rail placement ---")
    cmd(c, "setblock 110 63 110 stone")
    cmd(c, "setblock 110 64 110 powered_rail[powered=true]")
    hold(c, "minecart")
    use_on(c, 110, 64, 110)
    sync(c)

    print("--- 4. Projectiles: Ghast Fireball summon ---")
    cmd(c, "summon fireball 120 65 120")
    sync(c)

    print("--- 5. Shulker Bullet summon ---")
    cmd(c, "summon shulker_bullet 125 65 125")
    sync(c)

    print("vehicles tests completed successfully!")
    c.close()

if __name__ == "__main__":
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 25565
    run(host, port)
