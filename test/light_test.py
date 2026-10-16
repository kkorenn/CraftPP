#!/usr/bin/env python3
"""Incremental light on the flat test world (grass at y -61): a torch, a shaft dug to bedrock, then the torch
removed. Light is read from fresh chunk packets (vanilla only sends light_update to view-border players).
  python3 test/light_test.py host port"""
import os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from bot import *

def light(c, kind, x, y, z):
    d = c.light[x >> 4, z >> 4][kind].get((y >> 4) + 5, bytes(2048))
    i = (y & 15) << 8 | (z & 15) << 4 | (x & 15)
    return d[i >> 1] >> 4 * (i & 1) & 15

def snapshot(name):
    c, _ = login(name, host, port, view=4, decode=True)
    while (2, 2) not in c.light or (3, 2) not in c.light: play_step(c)
    c.s.close()
    return c

def run(c, *cmds):
    for line in cmds: c.send(PLAY_SB["chat_command"], string(line))
    wait(c, PLAY_CB["system_chat"])  # command feedback: the edits ran
    time.sleep(0.3)  # light runs at the end of the tick

host, port = sys.argv[1], int(sys.argv[2])
a, _ = login("Alice", host, port, view=4, decode=True)
run(a, "setblock 40 -60 40 torch", "fill 50 -63 40 50 -61 40 air")
s = snapshot("Lux1")
expect = {(40, -60, 40): 14, (41, -60, 40): 13, (45, -60, 40): 9, (40, -59, 40): 13, (40, -61, 40): 0, (40, -60, 54): 0}
got = {p: light(s, "block", *p) for p in expect}
assert got == expect, f"block light {got}"
sky = {p: light(s, "sky", *p) for p in [(50, -63, 40), (50, -62, 40), (51, -63, 40), (50, -64, 40), (40, -60, 40)]}
assert sky == {(50, -63, 40): 15, (50, -62, 40): 15, (51, -63, 40): 0, (50, -64, 40): 0, (40, -60, 40): 15}, f"sky light {sky}"
run(a, "setblock 40 -60 40 air")
s = snapshot("Lux2")
assert all(light(s, "block", *p) == 0 for p in expect), "torch light not removed"
a.s.close()
print("light ok: torch 14 -> 9 at 5 blocks, shaft sky 15, removal clears")
