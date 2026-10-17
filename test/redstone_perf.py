#!/usr/bin/env python3
"""Level tick cost of redstone (server started with CRAFTPP_LEVEL_PROFILE=1; reads the averages from its log).
Phases: idle, an observer clock, the clock driving a 100-wire line (a repeater every 15 wires).
Usage: python3 test/redstone_perf.py host port server.log"""
import os, re, sys, time, uuid
sys.path.insert(0, os.path.dirname(__file__))
from bot import *

host, port, log = sys.argv[1], int(sys.argv[2]), sys.argv[3]
c, _ = login("Alice", host, port, decode=False, uid=uuid.uuid3(uuid.NAMESPACE_DNS, "Alice"))
def cmd(s): c.send(PLAY_SB["chat_command"], string(s))
def pump(sec):
    end = time.time() + sec
    while time.time() < end:
        try: c.recv()
        except Exception: pass
def averages():
    return [float(m) for m in re.findall(r"level tick avg ([0-9.]+) ms", open(log).read())]
def phase(name, sec=25):
    n0 = len(averages())
    pump(sec)
    v = averages()[n0 + 1:]  # skip the first (partial) window
    print(f"{name}: {sum(v) / max(1, len(v)):.4f} ms/tick (windows {[round(x, 4) for x in v]})")

X, Y, Z = 8, -60, 900
cmd("gamemode creative")
cmd(f"tp Alice {X - 45} {Y + 3} {Z + 4}")
pump(5)
phase("idle")
cmd(f"setblock {X} {Y} {Z} observer[facing=east]")
cmd(f"setblock {X + 1} {Y} {Z} observer[facing=west]")
phase("observer clock")
for i in range(1, 101):  # line west of observer A (its output side) at x = X-1 .. X-100
    x = X - i
    cmd(f"setblock {x} {Y} {Z} " + ("repeater[facing=east]" if i % 15 == 0 else "redstone_wire"))
pump(2)
phase("clock + 100-wire line")
c.s.close()
