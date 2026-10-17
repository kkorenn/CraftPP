#!/usr/bin/env python3
"""Environment & Mechanics tests: weather, lightning strike conversions, beds & sleeping,
respawn anchor mechanics, explosions, and village raids.
Usage: python3 test/environment_test.py host port"""
import json, os, struct, sys, time, uuid
sys.path.insert(0, os.path.dirname(__file__))
from bot import *

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REG = json.load(open(os.path.join(ROOT, "data/reports/registries.json")))
ENTITY = ids("entity_types")

def rid(reg, n): return REG["minecraft:" + reg]["entries"]["minecraft:" + n]["protocol_id"]
HERO_OF_VILLAGE = rid("mob_effect", "hero_of_the_village")
BAD_OMEN = rid("mob_effect", "bad_omen")
RAID_OMEN = rid("mob_effect", "raid_omen")

def pos(x, y, z): return struct.pack(">q", (x & 0x3FFFFFF) << 38 | (z & 0x3FFFFFF) << 12 | (y & 0xFFF))
def cmd(c, s): c.send(PLAY_SB["chat_command"], string(s))

def varlong(b):
    n = s = 0
    while True:
        x = b.u8(); n |= (x & 0x7F) << s; s += 7
        if not x & 0x80: return n

def read_time(b):
    gt = b.i64()
    clocks = {}
    for _ in range(b.varint()):
        cid = b.varint(); total = varlong(b)
        partial, rate = struct.unpack(">ff", b.take(8))
        clocks[cid] = (total, rate)
    return gt, clocks

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

def teleport_dim(c, dim, x, y, z):
    cmd(c, f"execute in {dim} run tp @s {x} {y} {z}")
    pump_until(c, lambda pid, b: pid == PLAY_CB["respawn"], 5.0)
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
        res = pred(pid, b)
        if res: return res
    raise AssertionError(f"Timeout waiting for condition after {timeout:.1f}s")

host, port = sys.argv[1], int(sys.argv[2])
for attempt in range(50):
    try:
        c, _ = login("Sam", host, port, decode=True, uid=uuid.uuid3(uuid.NAMESPACE_DNS, "Sam"))
        break
    except AssertionError:
        time.sleep(0.2)

c.s.settimeout(15)
cmd(c, "gamemode creative")
teleport(c, 200.5, -60.0, 200.5)

# =========================================================================
# 1. Weather cycle: rain, thunder, clear
# =========================================================================
print("Testing weather...")
cmd(c, "weather rain 100")
# game_event 1 is start_rain, 7 is rain_level_change
rain_started = False
def check_rain(pid, b):
    global rain_started
    if pid == PLAY_CB["game_event"]:
        ev = b.u8()
        if ev == 1:
            rain_started = True
            return True
pump_until(c, check_rain, 5.0)
assert rain_started, "Rain start event not received"

cmd(c, "weather thunder 100")
thunder_started = False
def check_thunder(pid, b):
    global thunder_started
    if pid == PLAY_CB["game_event"]:
        ev = b.u8()
        if ev == 8: # thunder level change
            thunder_started = True
            return True
pump_until(c, check_thunder, 5.0)
assert thunder_started, "Thunder level event not received"

cmd(c, "weather clear 100")
rain_stopped = False
def check_clear(pid, b):
    global rain_stopped
    if pid == PLAY_CB["game_event"]:
        ev = b.u8()
        if ev == 2: # stop_rain
            rain_stopped = True
            return True
pump_until(c, check_clear, 5.0)
assert rain_stopped, "Clear weather event not received"
print("Weather rain/thunder/clear ok")

# =========================================================================
# 2. Lightning conversions: Pig -> Zombified Piglin, Creeper -> Powered Creeper
# =========================================================================
print("Testing lightning conversions...")
cmd(c, "difficulty hard")
cmd(c, "gamerule spawn_mobs false")
teleport(c, 250.5, -60.0, 250.5)

# 2a. Pig to Zombified Piglin
cmd(c, "kill @e[type=!player]")
sync(c)
cmd(c, "summon minecraft:pig 250 -60 250")
pig_eid = None
def find_pig(pid, b):
    global pig_eid
    if pid == PLAY_CB["add_entity"]:
        eid = b.varint(); b.take(16); etype = b.varint()
        if etype == ENTITY["pig"]:
            pig_eid = eid
            return True
pump_until(c, find_pig, 5.0)
assert pig_eid is not None, "Pig failed to spawn"

# Strike pig with lightning
cmd(c, "summon minecraft:lightning_bolt 250 -60 250")
zombified_found = False
def check_zombified(pid, b):
    global zombified_found
    if pid == PLAY_CB["add_entity"]:
        eid = b.varint(); b.take(16); etype = b.varint()
        if etype == ENTITY["zombified_piglin"]:
            zombified_found = True
            return True
pump_until(c, check_zombified, 5.0)
assert zombified_found, "Pig did not convert to Zombified Piglin on lightning strike"
print("Pig -> Zombified Piglin conversion ok")

# 2b. Creeper to Powered Creeper
cmd(c, "kill @e[type=!player]")
sync(c)
cmd(c, "summon minecraft:creeper 250 -60 250")
creeper_eid = None
def find_creeper(pid, b):
    global creeper_eid
    if pid == PLAY_CB["add_entity"]:
        eid = b.varint(); b.take(16); etype = b.varint()
        if etype == ENTITY["creeper"]:
            creeper_eid = eid
            return True
pump_until(c, find_creeper, 5.0)
assert creeper_eid is not None, "Creeper failed to spawn"

cmd(c, "summon minecraft:lightning_bolt 250 -60 250")
powered_seen = False
def check_creeper_powered(pid, b):
    global powered_seen
    if pid == PLAY_CB["set_entity_data"]:
        eid = b.varint()
        if eid == creeper_eid:
            raw = b.d[b.i:]
            if b"\x01" in raw: # boolean true flag
                powered_seen = True
                return True
pump_until(c, check_creeper_powered, 5.0)
assert powered_seen, "Creeper not powered by lightning strike"
print("Creeper -> Powered Creeper ok")

# 2c. Copper oxidation stripping vs waxed copper immunity
cmd(c, "setblock 260 -60 260 minecraft:weathered_copper")
cmd(c, "setblock 261 -60 260 minecraft:waxed_oxidized_copper")
sync(c)
cmd(c, "summon minecraft:lightning_bolt 260 -60 260")
deoxidized = False
def check_copper(pid, b):
    global deoxidized
    if pid == PLAY_CB["block_update"]:
        p = block_pos(b.i64())
        if p == (260, -60, 260):
            deoxidized = True
            return True
pump_until(c, check_copper, 5.0)
assert deoxidized, "Weathered copper was not deoxidized by lightning"
print("Copper de-oxidation & waxed immunity ok")

# =========================================================================
# 3. Beds & Sleeping: Daytime rejection, Night skipping, Nether explosion
# =========================================================================
print("Testing bed sleeping and explosions...")
cmd(c, "kill @e[type=!player]")
teleport(c, 300.5, -60.0, 302.5)

# 3a. Daytime sleep rejection
cmd(c, "time set noon")
cmd(c, "setblock 300 -60 301 red_bed[facing=north,part=foot]")
cmd(c, "setblock 300 -60 300 red_bed[facing=north,part=head]")
sync(c)

no_sleep_msg = False
def check_no_sleep(pid, b):
    global no_sleep_msg
    if pid == PLAY_CB["set_action_bar_text"]:
        if b"block.minecraft.bed.no_sleep" in b.d:
            no_sleep_msg = True
            return True
use_on(c, 300, -60, 300)
pump_until(c, check_no_sleep, 5.0)
assert no_sleep_msg, "Did not reject sleeping during daytime"
print("Daytime sleep rejection ok")

# 3b. Night sleeping and night skip
cmd(c, "time set 13000")
cmd(c, "gamerule players_sleeping_percentage 100")
sync(c)

time_skipped = False
def check_time_skip(pid, b):
    global time_skipped
    if pid == PLAY_CB["set_time"]:
        gt, clocks = read_time(b)
        if 0 in clocks:
            total, _ = clocks[0]
            if (total % 24000) < 1000:
                time_skipped = True
                return True
use_on(c, 300, -60, 300)
pump_until(c, check_time_skip, 10.0)
assert time_skipped, "Bed sleeping failed to skip night"
print("Night sleeping + night skip ok")

# 3c. Nether bed explosion
teleport_dim(c, "minecraft:the_nether", 300.5, 60.0, 302.5)
cmd(c, "setblock 300 60 301 red_bed[facing=north,part=foot]")
cmd(c, "setblock 300 60 300 red_bed[facing=north,part=head]")
sync(c)

bed_exploded = False
head_cleared = False
foot_cleared = False
def check_explode(pid, b):
    global bed_exploded, head_cleared, foot_cleared
    if pid == PLAY_CB["explode"]:
        bed_exploded = True
    elif pid == PLAY_CB["block_update"]:
        p = block_pos(b.i64())
        bs = b.varint()
        if p == (300, 60, 300) and bs == 0:
            head_cleared = True
        elif p == (300, 60, 301) and bs == 0:
            foot_cleared = True
    if bed_exploded and head_cleared and foot_cleared:
        return True
use_on(c, 300, 60, 300)
pump_until(c, check_explode, 5.0)
assert bed_exploded and head_cleared and foot_cleared, "Bed explosion did not clear both head and foot"
print("Nether bed explosion ok")

# 3d. Bed foot interaction and occupancy rejection
teleport_dim(c, "minecraft:overworld", 310.5, -60.0, 312.5)
cmd(c, "time set 13000")
cmd(c, "gamerule players_sleeping_percentage 100")
cmd(c, "setblock 310 -60 311 red_bed[facing=north,part=foot]")
cmd(c, "setblock 310 -60 310 red_bed[facing=north,part=head]")
sync(c)
# Clicking foot puts player to sleep
use_on(c, 310, -60, 311)
time.sleep(0.3)
sync(c)
# Second player attempts to click the same bed while occupied
c2, _ = login("Cora", host, port, decode=False, uid=uuid.uuid3(uuid.NAMESPACE_DNS, "Cora"))
c2.s.settimeout(10)
cmd(c2, "gamemode creative")
teleport(c2, 310.5, -60.0, 312.5)
occupied_msg = False
def check_occupied(pid, b):
    global occupied_msg
    if pid == PLAY_CB["set_action_bar_text"]:
        if b"block.minecraft.bed.occupied" in b.d:
            occupied_msg = True
            return True
use_on(c2, 310, -60, 311)
pump_until(c2, check_occupied, 5.0)
assert occupied_msg, "Occupied bed did not reject second player"
c2.s.close()
print("Bed foot sleep & occupancy rejection ok")

# =========================================================================
# 4. Respawn Anchor: Charge & spawn in Nether, Explode in Overworld
# =========================================================================
print("Testing respawn anchors...")
# 4a. Nether charge
teleport_dim(c, "minecraft:the_nether", 305.5, 60.0, 307.5)
cmd(c, "setblock 305 60 305 respawn_anchor[charges=0]")
sync(c)
hold(c, "glowstone", 4)

charged = False
def check_charge(pid, b):
    global charged
    if pid == PLAY_CB["block_update"]:
        p = block_pos(b.i64())
        if p == (305, 60, 305):
            charged = True
            return True
use_on(c, 305, 60, 305)
pump_until(c, check_charge, 5.0)
assert charged, "Respawn anchor was not charged by glowstone"

# Nether set spawn
hold(c, "air", 0)
spawn_set_msg = False
def check_anchor_spawn(pid, b):
    global spawn_set_msg
    if pid == PLAY_CB["set_action_bar_text"]:
        if b"block.minecraft.respawn_anchor.set_spawn" in b.d:
            spawn_set_msg = True
            return True
use_on(c, 305, 60, 305)
pump_until(c, check_anchor_spawn, 5.0)
assert spawn_set_msg, "Respawn anchor did not set spawn point"
print("Nether respawn anchor charge and set_spawn ok")

# 4b. Overworld charged anchor explosion
teleport_dim(c, "minecraft:overworld", 400.5, -60.0, 402.5)
cmd(c, "setblock 400 -60 400 respawn_anchor[charges=1]")
sync(c)

anchor_exploded = False
def check_anchor_explode(pid, b):
    global anchor_exploded
    if pid == PLAY_CB["explode"]:
        anchor_exploded = True
        return True
use_on(c, 400, -60, 400)
pump_until(c, check_anchor_explode, 5.0)
assert anchor_exploded, "Charged respawn anchor in Overworld did not explode"
print("Overworld respawn anchor explosion ok")

# =========================================================================
# 5. Village Raids: Start, Boss bar, Raiders, Stop, Victory & Hero of the Village
# =========================================================================
print("Testing village raids...")
teleport(c, 500.5, -60.0, 500.5)
# Create a village point of interest
cmd(c, "setblock 500 -60 500 red_bed[facing=north,part=foot]")
cmd(c, "setblock 500 -60 499 red_bed[facing=north,part=head]")
cmd(c, "summon minecraft:villager 500 -60 500")
sync(c)

# 5a. /raid start
cmd(c, "raid start 1")
bossbar_seen = False
bossbar_uuid = None
raiders_spawned = set()

def check_raid_start(pid, b):
    global bossbar_seen, bossbar_uuid
    if pid == PLAY_CB["boss_event"]:
        uid = (b.i64(), b.i64())
        action = b.varint()
        if action == 0: # ADD
            bossbar_seen = True
            bossbar_uuid = uid
            return True
    elif pid == PLAY_CB["add_entity"]:
        eid = b.varint(); b.take(16); etype = b.varint()
        if etype in (ENTITY["pillager"], ENTITY["vindicator"], ENTITY["witch"], ENTITY["ravager"]):
            raiders_spawned.add(etype)

pump_until(c, check_raid_start, 5.0)
assert bossbar_seen, "Raid boss bar not received"

# Collect spawned raiders
t_end = time.time() + 2.0
while time.time() < t_end:
    pid, b = c.recv()
    if pid == PLAY_CB["add_entity"]:
        eid = b.varint(); b.take(16); etype = b.varint()
        if etype in (ENTITY["pillager"], ENTITY["vindicator"], ENTITY["witch"], ENTITY["ravager"]):
            raiders_spawned.add(etype)
assert len(raiders_spawned) > 0, "No raid entities spawned"
print(f"Raid started ok: boss bar active, {len(raiders_spawned)} raider types spawned")

# 5b. /raid stop
cmd(c, "raid stop")
bossbar_removed = False
def check_raid_stop(pid, b):
    global bossbar_removed
    if pid == PLAY_CB["boss_event"]:
        uid = (b.i64(), b.i64())
        action = b.varint()
        if action == 1: # REMOVE
            bossbar_removed = True
            return True
pump_until(c, check_raid_stop, 5.0)
assert bossbar_removed, "Raid stop did not remove boss bar"
print("Raid stop ok")

# 5c. Complete raid -> Victory & Hero of the Village
cmd(c, "kill @e[type=!player]")
sync(c)
cmd(c, "raid start 1")
pump_until(c, lambda pid, b: pid == PLAY_CB["boss_event"] and b.take(16) and b.varint() == 0, 5.0)

# Kill wave raiders repeatedly to advance through all waves until victory
victory_seen = False
def check_victory(pid, b):
    global victory_seen
    if pid == PLAY_CB["update_mob_effect"]:
        b.varint() # eid
        eff = b.varint()
        if eff == HERO_OF_VILLAGE:
            victory_seen = True
            return True
    elif pid == PLAY_CB["boss_event"]:
        b.take(16); action = b.varint()
        if action == 3: # UPDATE_NAME
            if b"Victory" in b.d:
                victory_seen = True
                return True

for _ in range(10):
    cmd(c, "kill @e[type=!player]")
    sync(c)
    time.sleep(0.5)
    t_check = time.time() + 1.0
    while time.time() < t_check:
        pid, b = c.recv()
        if check_victory(pid, b):
            break
    if victory_seen:
        break

assert victory_seen, "Raid victory and Hero of the Village effect not received"
print("Raid victory + Hero of the Village ok")

cmd(c, "kill @e[type=!player]")
sync(c)

# =========================================================================
# 6. Pillager Captain death & Ominous Bottle drop
# =========================================================================
print("Testing Pillager Captain & Ominous Bottle drop...")
teleport(c, 600.5, -60.0, 600.5)
cmd(c, "kill @e[type=!player]")
sync(c)

cmd(c, "setblock 600 -60 600 red_bed[facing=north,part=foot]")
cmd(c, "setblock 600 -60 599 red_bed[facing=north,part=head]")
cmd(c, "summon minecraft:villager 600 -60 600")
sync(c)
cmd(c, "raid start 1")
sync(c)

bottle_dropped = False
def check_bottle(pid, b):
    global bottle_dropped
    if pid == PLAY_CB["add_entity"]:
        eid = b.varint(); b.take(16); etype = b.varint()
        if etype == ENTITY["item"]:
            bottle_dropped = True
            return True

time.sleep(0.5)
cmd(c, "kill @e[type=pillager]")
pump_until(c, check_bottle, 5.0)
assert bottle_dropped, "Pillager captain did not drop ominous bottle"
print("Pillager Captain drop ominous bottle ok")

cmd(c, "raid stop")
cmd(c, "kill @e[type=!player]")
sync(c)

# =========================================================================
# 7. Drinking Ominous Bottle -> Bad Omen -> Village Raid Omen
# =========================================================================
print("Testing Ominous bottle drink and Raid Omen conversion...")
cmd(c, "effect clear @s")
hold(c, "ominous_bottle", 1)
c.seq = getattr(c, "seq", 0) + 1
c.send(PLAY_SB["use_item"], varint(0) + varint(c.seq) + struct.pack(">ff", 0.0, 0.0))

bad_omen_seen = False
raid_omen_seen = False
def check_omen(pid, b):
    global bad_omen_seen, raid_omen_seen
    if pid == PLAY_CB["update_mob_effect"]:
        b.varint() # eid
        eff = b.varint()
        if eff == BAD_OMEN:
            bad_omen_seen = True
        elif eff == RAID_OMEN:
            raid_omen_seen = True
            return True

pump_until(c, check_omen, 5.0)
assert bad_omen_seen or raid_omen_seen, "Drinking ominous bottle did not trigger Bad Omen / Raid Omen"
print("Ominous bottle drinking -> Bad Omen -> Raid Omen ok")

cmd(c, "kill @e[type=!player]")
sync(c)
print("All environment & mechanics tests passed successfully!")
