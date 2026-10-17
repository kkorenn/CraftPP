#!/usr/bin/env python3
"""Block ticks: water spreading on flat ground (vanilla pattern and timing), lava + water -> cobblestone,
sand falling as an entity, door placement (both halves) and support rules.
Usage: python3 test/level_test.py host port  (flat creative test world, Alice is op)"""
import json, os, struct, sys, time, uuid
sys.path.insert(0, os.path.dirname(__file__))
from bot import *
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORT = json.load(open(os.path.join(ROOT, "data/reports/blocks.json")))
ITEMS = json.load(open(os.path.join(ROOT, "data/reports/registries.json")))["minecraft:item"]["entries"]
ENTITY = ids("entity_types")

def state(name, **props):
    for s in REPORT["minecraft:" + name]["states"]:
        if all(s.get("properties", {}).get(k) == str(v).lower() for k, v in props.items()) and (props or s.get("default")):
            return s["id"]
    raise KeyError(name, props)
def base(name): return min(s["id"] for s in REPORT["minecraft:" + name]["states"])
def pos(x, y, z): return struct.pack(">q", (x & 0x3FFFFFF) << 38 | (z & 0x3FFFFFF) << 12 | (y & 0xFFF))

WATER, LAVA, AIR = base("water"), base("lava"), state("air")
host, port = sys.argv[1], int(sys.argv[2])
c, _ = login("Alice", host, port, decode=True, uid=uuid.uuid3(uuid.NAMESPACE_DNS, "Alice"))
c.s.settimeout(10)
c.entities = {}
game_times = []

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
        elif pid == PLAY_CB["add_entity"]:
            eid = b.varint(); b.take(16); c.entities[eid] = b.varint()
        elif pid == PLAY_CB["system_chat"] and b"commands.time.query.gametime" in b.d:
            marker = b"\x08\x00\x04text"
            value = Buf(b.d[b.d.index(marker) + len(marker):])
            game_times.append(int(value.take(value.u16()).decode()))

def cmd(s):
    c.send(PLAY_SB["chat_command"], string(s))

def wait_until(pred, seconds, what):
    end = time.time() + seconds
    while time.time() < end:
        if pred(): return time.time()
        pump(0.05)
    raise AssertionError(what)

def query_game_time():
    count = len(game_times)
    cmd("time query gametime")
    wait_until(lambda: len(game_times) > count, 15, "game time response")
    return game_times[-1]

cmd("gamemode creative")
pump(1.0)

# 1. Water source on flat grass: spreads to level 7 in a diamond (manhattan distance d -> level d), 5 ticks a step.
X, Y, Z = 8, -60, 300  # away from the other tests (light_test reads block light near 40, 40)
cmd(f"tp Alice {X} {Y + 3} {Z - 12}")
need = [((X + dx) >> 4, (Z + dz) >> 4) for dx in (-9, 9) for dz in (-9, 9)]
wait_until(lambda: all(k in c.chunks for k in need), 10, f"chunks around the water test: missing {[k for k in need if k not in c.chunks]}, at {getattr(c, 'pos', None)}")
pump(0.5)
tick0 = query_game_time()
t0 = time.time()
cmd(f"setblock {X} {Y} {Z} water")
wait_until(lambda: at(X, Y, Z) == WATER, 10, "water source placed")
wait_until(lambda: at(X + 7, Y, Z) == WATER + 7, 15, "water should reach distance 7")
dt = time.time() - t0
elapsed_ticks = query_game_time() - tick0
pump(1.0)
bad = []
for dx in range(-9, 10):
    for dz in range(-9, 10):
        d = abs(dx) + abs(dz)
        want = WATER + d if d <= 7 else AIR
        got = at(X + dx, Y, Z + dz)
        if got != want: bad.append((dx, dz, got, want))
assert not bad, f"water pattern differs at {bad[:5]}"
# Measured in game ticks (catch-up after lag compresses wall time); the upper bound also covers two command round
# trips on a loaded host, so it only catches spreading that is several times too slow.
assert 30 <= elapsed_ticks <= 140, f"water spread took {elapsed_ticks} ticks (vanilla: 7 x 5 = 35 ticks)"
print(f"water: diamond to level 7 in {elapsed_ticks} ticks ({dt:.2f}s)")

# 2. Flowing lava touching water turns into cobblestone (LiquidBlock.shouldSpreadLiquid).
LX, LZ = 30, 300
cmd(f"setblock {LX} {Y} {LZ} lava")
wait_until(lambda: at(LX + 1, Y, LZ) == LAVA + 2, 4, "lava should flow one block (level 2, 30 ticks)")
cmd(f"setblock {LX + 2} {Y} {LZ} water")
wait_until(lambda: at(LX + 1, Y, LZ) == state("cobblestone"), 3, "flowing lava next to water -> cobblestone")
assert at(LX, Y, LZ) in (LAVA, state("obsidian")), "lava source"
print("lava + water: cobblestone")

# 3. Sand in the air falls as a falling_block entity and lands on the grass.
SX, SZ = 50, 300
cmd(f"setblock {SX} {Y + 6} {SZ} sand")
wait_until(lambda: ENTITY["falling_block"] in c.entities.values(), 3, "falling_block entity")
wait_until(lambda: at(SX, Y, SZ) == state("sand") and at(SX, Y + 6, SZ) == AIR, 4, "sand should land on the ground")
print("sand: fell and landed")
# 4. Placing a door places both halves (DoorBlock.setPlacedBy); breaking the lower half removes the upper one.
DX, DZ = 70, 300
cmd(f"tp Alice {DX + 0.5} {Y} {DZ - 2.5}")
pump(0.5)
c.send(PLAY_SB["set_creative_mode_slot"], struct.pack(">h", 36) + varint(1) + varint(ITEMS["minecraft:oak_door"]["protocol_id"]) + varint(0) + varint(0))
c.send(PLAY_SB["use_item_on"], varint(0) + pos(DX, Y - 1, DZ) + varint(1) + struct.pack(">fff", .5, 1, .5) + b"\0\0" + varint(50))
lower = state("oak_door", half="lower", facing="south", hinge="left", open=False, powered=False)
upper = state("oak_door", half="upper", facing="south", hinge="left", open=False, powered=False)
wait_until(lambda: at(DX, Y, DZ) == lower and at(DX, Y + 1, DZ) == upper, 3, f"door halves: {at(DX, Y, DZ)} {at(DX, Y + 1, DZ)}")
c.send(PLAY_SB["use_item_on"], varint(0) + pos(DX, Y, DZ) + varint(1) + struct.pack(">fff", .5, .5, .5) + b"\0\0" + varint(51))
wait_until(lambda: at(DX, Y, DZ) == state("oak_door", half="lower", facing="south", hinge="left", open=True, powered=False), 3, "door opens")
c.send(PLAY_SB["player_action"], varint(0) + pos(DX, Y, DZ) + b"\x01" + varint(52))
wait_until(lambda: at(DX, Y, DZ) == AIR and at(DX, Y + 1, DZ) == AIR, 3, "breaking the lower half removes the door")
print("door: both halves placed, opened, broken together")

# 5. A torch pops off when its support goes.
TX = 74
cmd(f"setblock {TX} {Y} {DZ} dirt")
cmd(f"setblock {TX} {Y + 1} {DZ} torch")
wait_until(lambda: at(TX, Y + 1, DZ) == state("torch"), 3, "torch placed")
cmd(f"setblock {TX} {Y} {DZ} air")
wait_until(lambda: at(TX, Y + 1, DZ) == AIR, 3, "torch should pop off")
print("torch: popped off without support")
# 6. Water bucket: BucketItem.use places a source where the player looks; an empty bucket picks it up again.
BX, BZ = 80, 300
cmd(f"tp Alice {BX + 0.5} {Y} {BZ + 0.5}")
pump(0.5)
c.send(PLAY_SB["set_creative_mode_slot"], struct.pack(">h", 36) + varint(1) + varint(ITEMS["minecraft:water_bucket"]["protocol_id"]) + varint(0) + varint(0))
c.send(PLAY_SB["use_item"], varint(0) + varint(60) + struct.pack(">ff", 0, 90))
wait_until(lambda: at(BX, Y, BZ) == WATER, 3, f"bucket should place water, got {at(BX, Y, BZ)}")
c.send(PLAY_SB["set_creative_mode_slot"], struct.pack(">h", 36) + varint(1) + varint(ITEMS["minecraft:bucket"]["protocol_id"]) + varint(0) + varint(0))
c.send(PLAY_SB["use_item"], varint(0) + varint(61) + struct.pack(">ff", 0, 90))
wait_until(lambda: at(BX, Y, BZ) == AIR, 3, "empty bucket should pick the source up")
print("bucket: placed and picked up water")
c.s.close()
print("level test passed")
