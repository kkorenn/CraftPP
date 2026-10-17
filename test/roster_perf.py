#!/usr/bin/env python3
"""Roster perf probe (not part of ./build.sh test): N mobs, half villagers (with job sites), half animals; reports
server mspt / heap from CRAFTPP_MOBDEBUG=1 log lines. Usage: python3 test/roster_perf.py host port logfile [N]"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from bot import *

def cmd(c, line): c.send(PLAY_SB["chat_command"], string(line))
def drain(c, seconds):
    end = time.time() + seconds
    while time.time() < end: c.recv()
def last_stats(log):
    lines = [l for l in open(log, errors="replace") if l.startswith("[mobs] tick")]
    m = re.search(r"entities (\d+) active (\d+) mspt ([\d.]+) rss (\d+) KiB heap (\d+)", lines[-1])
    return int(m.group(1)), int(m.group(2)), float(m.group(3)), int(m.group(5))

host, port, log = sys.argv[1], int(sys.argv[2]), sys.argv[3]
n = int(sys.argv[4]) if len(sys.argv) > 4 else 100
c, _ = login("Sam", host, port)
cmd(c, "gamerule spawn_mobs false")
cmd(c, "time set midnight")
cmd(c, "gamerule advance_time false")
cmd(c, "kill @e[type=!player]")
drain(c, 12)
e0, a0, ms0, rss0 = last_stats(log)
kinds = ["villager", "cow", "villager", "sheep", "villager", "wolf", "villager", "horse", "villager", "rabbit"]
for i in range(0, n, 2):  # a workstation for every villager
    side = max(1, int(n ** 0.5))
    jobs = ["composter", "lectern", "barrel", "smoker", "blast_furnace", "loom", "fletching_table", "stonecutter"]
    cmd(c, f"setblock {int((i % side) * 64.0 / side - 32) + 1} -60 {int((i // side) * 64.0 / side - 32)} minecraft:{jobs[(i // 2) % len(jobs)]}")
for i in range(n):
    side = max(1, int(n ** 0.5))
    cmd(c, f"summon {kinds[i % len(kinds)]} {(i % side) * 64.0 / side - 32} -60 {(i // side) * 64.0 / side - 32}")
drain(c, 30)  # let the tick average settle (exponential average over ~100 ticks)
e1, a1, ms1, rss1 = last_stats(log)
print(f"{n} mobs: entities {e0}->{e1} active {a1}, mspt {ms0:.3f} -> {ms1:.3f}, heap {rss0} -> {rss1} KiB "
      f"({(rss1 - rss0) * 1024 / max(1, e1 - e0):.0f} B/entity incl. allocator)")
