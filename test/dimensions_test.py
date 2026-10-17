#!/usr/bin/env python3
"""Dimensions on the flat test world: nether portal lighting + travel + linking back, the survival delay,
dimension kept across rejoin, execute in, the end portal (eyes of ender), the end platform and the credits.
Usage: python3 test/dimensions_test.py host port"""
import json, os, socket, struct, sys, time, uuid
sys.path.insert(0, os.path.dirname(__file__))
from bot import *
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ITEMS = json.load(open(os.path.join(ROOT, "data/reports/registries.json")))["minecraft:item"]["entries"]
BLOCKS = json.load(open(os.path.join(ROOT, "data/reports/blocks.json")))
def state(name, **props):
    for s in BLOCKS["minecraft:" + name]["states"]:
        if all(s.get("properties", {}).get(k) == v for k, v in props.items()): return s["id"]
PORTAL = {state("nether_portal", axis="x"), state("nether_portal", axis="z")}
def pos(x, y, z): return struct.pack(">q", (x & 0x3FFFFFF) << 38 | (z & 0x3FFFFFF) << 12 | (y & 0xFFF))
def item(n): return ITEMS["minecraft:" + n]["protocol_id"]
def move(c, x, y, z): walk(c, x, y, z)
def cmd(c, line): c.send(PLAY_SB["chat_command"], string(line))
def hold(c, it): c.send(PLAY_SB["set_creative_mode_slot"], struct.pack(">h", 36) + varint(1) + varint(item(it)) + varint(0) + varint(0))
def use_on(c, x, y, z, face=1):
    c.seq = getattr(c, "seq", 0) + 1
    c.send(PLAY_SB["use_item_on"], varint(0) + pos(x, y, z) + varint(face) + struct.pack(">fff", .5, 1, .5) + b"\0\0" + varint(c.seq))
def block(c, x, y, z):  # -1 while the chunk hasn't arrived
    secs = c.chunks.get((x >> 4, z >> 4))
    if secs is None: return -1
    min_y = -64 if len(secs) == 24 else 0
    return secs[(y - min_y) >> 4][0][((y & 15) * 16 + (z & 15)) * 16 + (x & 15)]
def until(c, pred, limit=20000):
    for _ in range(limit):
        play_step(c)
        if pred(): return
    raise AssertionError("condition not reached")
def sync(c):  # everything the server sent before the reply to /list has been processed
    cmd(c, "list")
    until(c, lambda: c.last is not None and b"commands.list" in c.last.d)
def arrive(c, dim):  # a respawn packet into `dim`, then the teleport
    while not (play_step(c) == PLAY_CB["respawn"] and c.dimension == dim): pass
    while play_step(c) != PLAY_CB["player_position"]: pass

host, port = sys.argv[1], int(sys.argv[2])
ALICE = uuid.UUID("00000000-0000-4000-8000-00000000a11c")  # same profile across the rejoin
c, _ = login("Alice", host, port, decode=True, uid=ALICE)
c.last = None
cmd(c, "gamemode creative")
# Obsidian frame 4 wide x 5 tall along x at z = 5 (inside 2 x 3), standing on the flat grass (y -61).
cmd(c, "fill 10 -60 5 13 -56 5 minecraft:obsidian")
cmd(c, "fill 11 -59 5 12 -57 5 minecraft:air")
hold(c, "flint_and_steel")
move(c, 11.5, -60, 3.5)  # within reach
sync(c)
use_on(c, 11, -60, 5)  # top face of the bottom frame block: fire at 11 -59 5 lights the frame
until(c, lambda: all(block(c, x, y, 5) in PORTAL for x in (11, 12) for y in (-59, -58, -57)))
print("nether portal lit ok")

move(c, 11.5, -59, 5.5)  # creative: through at once
arrive(c, "minecraft:the_nether")
px, py, pz = c.pos
assert abs(px - 11.5 / 8) <= 18 and abs(pz - 5.5 / 8) <= 18, c.pos  # PortalForcer spiral radius 16
until(c, lambda: (int(px) >> 4, int(pz) >> 4) in c.chunks)
sync(c)
assert block(c, int(px // 1), int(py // 1), int(pz // 1)) in PORTAL, "arrived inside the created exit portal"
print(f"to the nether ok: {c.pos}")

c.s.shutdown(socket.SHUT_RDWR)  # rejoin: the player stays in the nether
for attempt in range(40):  # until the server has seen the old connection go
    time.sleep(0.25)
    try:
        c, _ = login("Alice", host, port, decode=True, uid=ALICE)
        break
    except AssertionError:
        pass
c.last = None
assert c.dimension == "minecraft:the_nether", c.dimension
assert abs(c.pos[0] - px) < 1e-6 and abs(c.pos[2] - pz) < 1e-6, c.pos
print("dimension kept across rejoin ok")

# Back through the same portal: links to the original frame. Survival waits 80 ticks.
cmd(c, "gamemode survival")
move(c, px, py, pz + 3)
time.sleep(1)  # off cooldown, out of the portal
def game_time():
    cmd(c, "time query gametime")
    until(c, lambda: c.last is not None and b"commands.time.query.gametime" in c.last.d)
    return int(text_of(c.last))
t0 = game_time()
move(c, px, py, pz)
arrive(c, "minecraft:overworld")
dt = game_time() - t0  # game ticks: lag catch-up makes wall time unreliable
assert dt >= 80, f"survival portal delay too short: {dt} ticks"
if abs(px * 8 - 11.5) <= 128 and abs(pz * 8 - 5.5) <= 128:  # the original frame is within the search radius
    assert 10 <= c.pos[0] <= 13 and abs(c.pos[2] - 5.5) < 1 and abs(c.pos[1] + 59) < 1, c.pos
print(f"back to the original portal ok ({dt} ticks survival delay): {c.pos}")

# Breaking the frame collapses the portal.
cmd(c, "gamemode creative")
cmd(c, "setblock 10 -58 5 minecraft:air")
until(c, lambda: all(block(c, x, y, 5) not in PORTAL | {-1} for x in (11, 12) for y in (-59, -58, -57)))
print("portal collapse ok")

# execute in: the source position scales by the coordinate scale.
ox, oz = c.pos[0], c.pos[2]  # the original frame, or a new one when the nether exit was out of its search radius
cmd(c, "execute in minecraft:the_nether run tp @s ~ 70 ~")
arrive(c, "minecraft:the_nether")
assert abs(c.pos[0] - ox / 8) < 1 and abs(c.pos[2] - oz / 8) < 1 and c.pos[1] == 70, c.pos
# Per-dimension ticks: lava reaches level 7 in the Nether (drop-off 1, ten ticks per step),
# while an Overworld client watching the same chunk coordinates receives no block changes.
observer, _ = login("DimensionWatch", host, port, view=2, decode=True)
until(observer, lambda: (0, 0) in observer.chunks)
cmd(c, "tp @s 8.5 248 8.5")
while play_step(c) != PLAY_CB["player_position"]: pass
until(c, lambda: all(k in c.chunks for k in ((0, 0), (1, 0), (0, 1))))
cmd(c, "fill 0 244 0 16 247 16 air")
cmd(c, "fill 0 244 0 16 244 16 stone")
sync(c)
cmd(c, "time query gametime")
until(c, lambda: b"commands.time.query.gametime" in c.last.d)
tick0 = int(text_of(c.last))
cmd(c, "setblock 8 245 8 lava")
until(c, lambda: block(c, 15, 245, 8) == state("lava", level="7"))
cmd(c, "time query gametime")
until(c, lambda: b"commands.time.query.gametime" in c.last.d)
elapsed = int(text_of(c.last)) - tick0
assert 60 <= elapsed <= 110, elapsed
observer.send(PLAY_SB["ping_request"], struct.pack(">q", 0x44494d))
while play_step(observer) != PLAY_CB["pong_response"]: pass
assert block(observer, 8, 245, 8) == state("air"), "Nether updates leaked into the Overworld"
observer.s.close()
print(f"Nether lava + dimension isolation ok: level 7 in {elapsed} ticks")
cmd(c, "locate structure minecraft:fortress")
reply = wait(c, PLAY_CB["system_chat"], lambda b: b"commands.locate.structure." in b.d)
assert b"commands.locate.structure.success" in reply.d and b"minecraft:fortress" in reply.d, reply.d
print("Nether structure manager locate ok")

cmd(c, "execute in minecraft:overworld run tp @s 0.5 -60 0.5")
arrive(c, "minecraft:overworld")
print("execute in + cross-dimension tp ok")

# End portal: 12 frames facing the 3x3 inside, the last eye opens it.
FRAMES = [(x, 20, "south") for x in (21, 22, 23)] + [(x, 24, "north") for x in (21, 22, 23)] + \
         [(20, z, "east") for z in (21, 22, 23)] + [(24, z, "west") for z in (21, 22, 23)]
for i, (x, z, f) in enumerate(FRAMES):
    cmd(c, f"setblock {x} -60 {z} minecraft:end_portal_frame[facing={f},eye={'false' if i == 0 else 'true'}]")
hold(c, "ender_eye")
move(c, 22.5, -60, 18.5)
sync(c)
use_on(c, 21, -60, 20)
END_PORTAL = state("end_portal")
until(c, lambda: all(block(c, x, -60, z) == END_PORTAL for x in (21, 22, 23) for z in (21, 22, 23)))
print("end portal opened ok")
time.sleep(1)  # portal cooldown from the last trip runs out (standing in a portal while on cooldown resets it)
move(c, 22.5, -60, 22.5)
arrive(c, "minecraft:the_end")
assert c.pos == (100.5, 49.0, 0.5), c.pos
until(c, lambda: (6, 0) in c.chunks)
sync(c)
assert block(c, 100, 48, 0) == state("obsidian") and block(c, 102, 48, 2) == state("obsidian"), "end platform"
assert block(c, 100, 49, 0) == state("air"), "end platform headroom"
print("to the end platform ok")

# Leaving the end: credits first (win_game), then the respawn at the world spawn.
cmd(c, "setblock 100 49 3 minecraft:end_portal")
sync(c)
time.sleep(1)
move(c, 100.5, 49, 3.5)
for _ in range(20000):
    pid, b = c.recv()
    if pid == PLAY_CB["game_event"] and b.d[b.i] == 4:
        b.u8(); assert struct.unpack(">f", b.take(4))[0] == 0.0  # WIN_GAME: 26.3 always sends 0
        break
c.send(PLAY_SB["client_command"], varint(0))  # credits done: PERFORM_RESPAWN
arrive(c, "minecraft:overworld")
print("credits + return to the overworld ok")
print("dimensions ok")
