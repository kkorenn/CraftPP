#!/usr/bin/env python3
"""Redstone contraptions against vanilla expectations: wire power falloff, repeater delay + lamp off-delay,
torch inverter, comparator compare/subtract, observer clock, trapdoor on power, piston push / retract and
sticky pull, powered rail propagation.
Usage: python3 test/redstone_test.py host port  (flat creative test world, Alice is op)"""
import os, sys, time, uuid
sys.path.insert(0, os.path.dirname(__file__))
from bot import *
import json
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORT = json.load(open(os.path.join(ROOT, "data/reports/blocks.json")))
BY_ID = {s["id"]: (n[10:], s.get("properties", {})) for n, b in REPORT.items() for s in b["states"]}

host, port = sys.argv[1], int(sys.argv[2])
c, _ = login("Alice", host, port, decode=True, uid=uuid.uuid3(uuid.NAMESPACE_DNS, "Alice"))
c.s.settimeout(10)
changes = {}  # (x, y, z) -> number of block updates seen

def at(x, y, z):
    secs = c.chunks.get((x >> 4, z >> 4))
    return secs[(y + 64) >> 4][0][((y & 15) * 16 + (z & 15)) * 16 + (x & 15)] if secs else None
def name(x, y, z): return BY_ID.get(at(x, y, z), ("?", {}))[0]
def prop(x, y, z, k): return BY_ID.get(at(x, y, z), ("?", {}))[1].get(k)

def upd(x, y, z, s):
    set_block(c, x, y, z, s)
    changes[x, y, z] = changes.get((x, y, z), 0) + 1

def pump(seconds):
    end = time.time() + seconds
    while time.time() < end:
        pid, b = c.recv()
        if pid == PLAY_CB["level_chunk_with_light"]:
            cx, cz, secs = parse_chunk(b, True)[:3]; c.chunks[cx, cz] = secs
        elif pid == PLAY_CB["block_update"]:
            upd(*block_pos(b.i64()), b.varint())
        elif pid == PLAY_CB["section_blocks_update"]:
            v = b.i64(); sx, sy, sz = v >> 42, sext(v, 20), sext(v >> 20, 22)
            for _ in range(b.varint()):
                e = b.varint()
                upd(sx * 16 + (e >> 8 & 15), sy * 16 + (e & 15), sz * 16 + (e >> 4 & 15), e >> 12)

def cmd(s): c.send(PLAY_SB["chat_command"], string(s))

def wait_until(pred, seconds, what):
    t0 = time.time()
    while time.time() < t0 + seconds:
        if pred(): return time.time() - t0
        pump(0.02)
    raise AssertionError(what)

X, Y, Z = 8, -60, 520
cmd("gamemode creative")
cmd(f"tp Alice {X} {Y + 3} {Z - 6}")
need = [((X + dx) >> 4, (Z + dz) >> 4) for dx in (-4, 24) for dz in (-4, 40)]
wait_until(lambda: all(k in c.chunks for k in need), 10, "chunks around the redstone test")
pump(0.5)

# 1. Wire line: a redstone block powers 16 wires 15..0 (DefaultRedstoneWireEvaluator); removing it unpowers all.
for i in range(16): cmd(f"setblock {X + i} {Y} {Z} redstone_wire")
pump(0.3)
cmd(f"setblock {X - 1} {Y} {Z} redstone_block")
wait_until(lambda: [prop(X + i, Y, Z, "power") for i in range(16)] == [str(15 - i) for i in range(16)], 3,
           f"wire falloff: {[prop(X + i, Y, Z, 'power') for i in range(16)]}")
cmd(f"setblock {X - 1} {Y} {Z} air")
wait_until(lambda: all(prop(X + i, Y, Z, "power") == "0" for i in range(16)), 3, "wire unpowered")
print("wire: 15..0 along 16 wires, off again")

# 2. Repeater (delay 4 = 8 ticks) into a lamp; lamp turns off 4 ticks after the repeater.
Z2 = Z + 4
cmd(f"setblock {X + 2} {Y} {Z2} redstone_lamp")
cmd(f"setblock {X + 1} {Y} {Z2} repeater[facing=west,delay=4]")
pump(0.3)
cmd(f"setblock {X} {Y} {Z2} redstone_block")
t_on = wait_until(lambda: prop(X + 2, Y, Z2, "lit") == "true", 3, "lamp lit through the repeater")
pump(0.5)
cmd(f"setblock {X} {Y} {Z2} air")
t_rep = wait_until(lambda: prop(X + 1, Y, Z2, "powered") == "false", 3, "repeater off")
t_off = wait_until(lambda: prop(X + 2, Y, Z2, "lit") == "false", 3, "lamp off")
assert 0.25 < t_on < 0.8, f"repeater delay 4 took {t_on:.2f}s (vanilla 8 ticks = 0.4 s)"
assert 0.1 < t_off < 0.5, f"lamp off-delay {t_off:.2f}s (vanilla 4 ticks = 0.2 s)"
print(f"repeater: on after {t_on:.2f}s, lamp off {t_off:.2f}s after the repeater")

# 3. Torch inverter: a repeater strongly powering the block a torch stands on turns the torch off.
#    (A redstone block next to the stone would not: it only powers weakly.)
Z3 = Z + 8
cmd(f"setblock {X} {Y} {Z3} stone")
cmd(f"setblock {X} {Y + 1} {Z3} redstone_torch")
cmd(f"setblock {X - 1} {Y} {Z3} repeater[facing=west]")
cmd(f"setblock {X - 3} {Y} {Z3} redstone_block")
wait_until(lambda: prop(X, Y + 1, Z3, "lit") == "true", 3, "torch placed lit")
pump(0.3)
assert prop(X, Y + 1, Z3, "lit") == "true", "a redstone block must not strongly power the stone"
cmd(f"setblock {X - 2} {Y} {Z3} redstone_block")
wait_until(lambda: prop(X, Y + 1, Z3, "lit") == "false", 3, "torch turned off by a powered block")
cmd(f"setblock {X - 2} {Y} {Z3} air")
wait_until(lambda: prop(X, Y + 1, Z3, "lit") == "true", 3, "torch back on")
print("torch: inverts")

# 4. Comparator: input 15 from a redstone block, side input 14 from a wire; subtract -> 1, compare -> 15.
Z4 = Z + 12
cmd(f"setblock {X + 1} {Y} {Z4 + 3} redstone_block")
cmd(f"setblock {X + 1} {Y} {Z4 + 2} redstone_wire")
cmd(f"setblock {X + 1} {Y} {Z4 + 1} redstone_wire")
cmd(f"setblock {X + 2} {Y} {Z4} redstone_wire")
cmd(f"setblock {X + 1} {Y} {Z4} comparator[facing=west,mode=subtract]")
pump(0.3)
cmd(f"setblock {X} {Y} {Z4} redstone_block")
wait_until(lambda: prop(X + 2, Y, Z4, "power") == "1", 3, f"subtract: output wire {prop(X + 2, Y, Z4, 'power')}")
cmd(f"setblock {X + 1} {Y} {Z4} comparator[facing=west,mode=compare]")
wait_until(lambda: prop(X + 2, Y, Z4, "power") == "15", 3, f"compare: output wire {prop(X + 2, Y, Z4, 'power')}")
print("comparator: subtract 15-14 = 1, compare 15")

# 5. Observer clock: two observers facing each other pulse forever.
Z5 = Z + 18
cmd(f"setblock {X} {Y} {Z5} observer[facing=east]")
cmd(f"setblock {X + 1} {Y} {Z5} observer[facing=west]")
pump(0.2)
n0 = changes.get((X, Y, Z5), 0)
pump(2.0)
n = changes.get((X, Y, Z5), 0) - n0
assert 8 <= n <= 30, f"observer clock toggled {n} times in 2 s (vanilla: on 2 ticks, off 2+ ticks)"
cmd(f"setblock {X + 1} {Y} {Z5} air")
print(f"observer clock: {n} toggles in 2 s")

# 6. Trapdoor opens on power.
Z6 = Z + 21
cmd(f"setblock {X} {Y} {Z6} oak_trapdoor")
pump(0.2)
cmd(f"setblock {X - 1} {Y} {Z6} redstone_block")
wait_until(lambda: prop(X, Y, Z6, "open") == "true" and prop(X, Y, Z6, "powered") == "true", 3, "trapdoor opens on power")
print("trapdoor: opens on power")

# 7. Piston pushes a stone; retracting leaves it; a sticky piston pulls it back.
for k, piston in enumerate(("piston", "sticky_piston")):
    Zp = Z + 24 + 3 * k
    cmd(f"setblock {X} {Y} {Zp} {piston}[facing=east]")
    cmd(f"setblock {X + 1} {Y} {Zp} stone")
    pump(0.3)
    cmd(f"setblock {X - 1} {Y} {Zp} redstone_block")
    wait_until(lambda: name(X + 2, Y, Zp) == "stone" and name(X + 1, Y, Zp) == "piston_head" and prop(X, Y, Zp, "extended") == "true",
               3, f"{piston} extends: {name(X, Y, Zp)} {name(X + 1, Y, Zp)} {name(X + 2, Y, Zp)}")
    pump(0.3)
    cmd(f"setblock {X - 1} {Y} {Zp} air")
    if piston == "piston":
        wait_until(lambda: prop(X, Y, Zp, "extended") == "false" and name(X + 1, Y, Zp) == "air" and name(X + 2, Y, Zp) == "stone",
                   3, f"piston retracts: {name(X, Y, Zp)} {name(X + 1, Y, Zp)} {name(X + 2, Y, Zp)}")
    else:
        wait_until(lambda: prop(X, Y, Zp, "extended") == "false" and name(X + 1, Y, Zp) == "stone" and name(X + 2, Y, Zp) == "air",
                   3, f"sticky piston pulls: {name(X, Y, Zp)} {name(X + 1, Y, Zp)} {name(X + 2, Y, Zp)}")
print("pistons: push, retract, sticky pull")

# 8. Powered rails: a redstone block powers 9 rails along the line (8 beyond the powered one).
Z8 = Z + 32
for i in range(11): cmd(f"setblock {X + i} {Y} {Z8} powered_rail[shape=east_west]")
pump(0.3)
cmd(f"setblock {X} {Y} {Z8 + 1} redstone_block")
wait_until(lambda: [prop(X + i, Y, Z8, "powered") for i in range(11)] == ["true"] * 9 + ["false"] * 2, 3,
           f"powered rail line: {[prop(X + i, Y, Z8, 'powered') for i in range(11)]}")
print("powered rails: 9 powered from one source")

c.s.close()
print("redstone test passed")
