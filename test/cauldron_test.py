#!/usr/bin/env python3
"""26.3 CauldronInteractions: bucket states, inventory results, sounds, and underwater rejection."""
import json, os, struct, sys, time, uuid
sys.path.insert(0, os.path.dirname(__file__))
from bot import *
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORT = json.load(open(os.path.join(ROOT, "data/reports/blocks.json")))
ITEMS = json.load(open(os.path.join(ROOT, "data/reports/registries.json")))["minecraft:item"]["entries"]
def item(name): return ITEMS["minecraft:" + name]["protocol_id"]
def state(name, **props):
    for s in REPORT["minecraft:" + name]["states"]:
        if (props and s.get("properties", {}) == {k: str(v) for k, v in props.items()}) or (not props and s.get("default")):
            return s["id"]
    raise KeyError((name, props))
def pos(x, y, z): return struct.pack(">q", (x & 0x3FFFFFF) << 38 | (z & 0x3FFFFFF) << 12 | (y & 0xFFF))
c, _ = login("Alice", sys.argv[1], int(sys.argv[2]), decode=True, uid=uuid.uuid3(uuid.NAMESPACE_DNS, "Alice"))
c.s.settimeout(10)
slots, sounds, pongs = {}, [], set()
def stack(b):
    n = b.varint()
    if not n: return (0, -1)
    i = b.varint()
    assert b.varint() == 0, "unexpected item components"
    assert b.varint() == 0, "unexpected removed components"
    return n, i

def pump(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        pid, b = c.recv()
        if pid == PLAY_CB["level_chunk_with_light"]:
            cx, cz, secs = parse_chunk(b, True)[:3]; c.chunks[cx, cz] = secs
        elif pid == PLAY_CB["block_update"]:
            set_block(c, *block_pos(b.i64()), b.varint())
        elif pid == PLAY_CB["section_blocks_update"]:
            v = b.i64(); sx, sy, sz = v >> 42, sext(v, 20), sext(v >> 20, 22)
            for _ in range(b.varint()):
                e = b.varint(); set_block(c, sx * 16 + (e >> 8 & 15), sy * 16 + (e & 15), sz * 16 + (e >> 4 & 15), e >> 12)
        elif pid == PLAY_CB["container_set_slot"]:
            b.varint(); b.varint(); i = b.i16(); slots[i] = stack(b)
        elif pid == PLAY_CB["container_set_content"]:
            b.varint(); b.varint()
            for i in range(b.varint()): slots[i] = stack(b)
        elif pid == PLAY_CB["pong_response"]:
            pongs.add(b.i64())
        elif pid == PLAY_CB["sound"]:
            if b.varint() == 0:
                sound = b.str()
                # Ambient mobs are independent of the interaction being tested.
                if sound.startswith("minecraft:item.bucket."): sounds.append(sound)

def wait_until(pred, message):
    end = time.monotonic() + 4
    while time.monotonic() < end:
        if pred(): return
        pump(.05)
    raise AssertionError(f"{message}; slots={slots}")
def sync():
    sync.seq += 1
    seq = sync.seq
    c.send(PLAY_SB["ping_request"], struct.pack(">q", seq))
    wait_until(lambda: seq in pongs, "server processed preceding requests")
sync.seq = 0
def cmd(s): c.send(PLAY_SB["chat_command"], string(s))
X, Y, Z = 8, -60, 700

def at(y=Y):
    secs = c.chunks.get((X >> 4, Z >> 4))
    return secs[(y + 64) >> 4][0][((y & 15) * 16 + (Z & 15)) * 16 + (X & 15)] if secs else None

def prepare(block, held, count=1, creative=False, hand=0):
    cmd("gamemode creative")
    cmd("clear @s")
    cmd(f"setblock {X} {Y} {Z} {block}")
    c.send(PLAY_SB["set_creative_mode_slot"], struct.pack(">h", 45 if hand else 36) + varint(count) + varint(item(held)) + varint(0) + varint(0))
    pump(.2)
    if not creative:
        cmd("gamemode survival")
        pump(.2)
    sync()
    pump(.05)
    sounds.clear()

def use(hand=0):
    use.seq += 1
    c.send(PLAY_SB["use_item_on"], varint(hand) + pos(X, Y, Z) + varint(1) + struct.pack(">fff", .5, 1, .5) + b"\0\0" + varint(use.seq))
use.seq = 0

cmd("gamemode creative")
cmd(f"tp @s {X + .5} {Y} {Z - 2.5}")
wait_until(lambda: at() is not None, "cauldron chunk loaded")
pump(.5)
for fluid in ("water", "lava", "powder_snow"):
    full = state(fluid + "_cauldron", **({"level": 3} if fluid != "lava" else {}))
    for initial in ("cauldron", "water_cauldron[level=1]", "lava_cauldron", "powder_snow_cauldron[level=2]"):
        prepare(initial, fluid + "_bucket")
        use()
        wait_until(lambda: at() == full and slots.get(36) == (1, item("bucket")), "fill " + initial + " with " + fluid)
        suffix = "" if fluid == "water" else "_" + fluid
        wait_until(lambda: "minecraft:item.bucket.empty" + suffix in sounds, "bucket empty sound")
        use()
        wait_until(lambda: at() == state("cauldron") and slots.get(36) == (1, item(fluid + "_bucket")), "extract " + fluid)
        wait_until(lambda: "minecraft:item.bucket.fill" + suffix in sounds, "bucket fill sound")
print("cauldron bucket fill/extract: all fluid replacements + sounds")

# A bucket cannot pick up partial layered cauldrons; no item consumed.
for fluid in ("water", "powder_snow"):
    for level in (1, 2):
        prepare(f"{fluid}_cauldron[level={level}]", "bucket", 3)
        use(); sync(); pump(.25)
        assert at() == state(fluid + "_cauldron", level=level)
        assert slots[36] == (3, item("bucket")) and not sounds, (fluid, level, slots.get(36), sounds)

# Stacked empty buckets leave the held remainder and add the filled bucket to inventory.
prepare("water_cauldron[level=3]", "bucket", 3)
use()
wait_until(lambda: at() == state("cauldron") and slots.get(36) == (2, item("bucket")) and (1, item("water_bucket")) in slots.values(), "stacked bucket remainder")

# Offhand uses the same block interaction dispatch.
prepare("cauldron", "lava_bucket", hand=1)
use(1)
wait_until(lambda: at() == state("lava_cauldron") and slots.get(45) == (1, item("bucket")), "offhand bucket")

# Creative keeps held buckets and creates at most one result stack.
prepare("water_cauldron[level=3]", "bucket", 4, creative=True)
use()
wait_until(lambda: at() == state("cauldron") and (1, item("water_bucket")) in slots.values(), "creative result")
assert slots[36] == (4, item("bucket"))
cmd(f"setblock {X} {Y} {Z} water_cauldron[level=3]")
pump(.2); use(); sync(); pump(.25)
assert sum(n for n, i in slots.values() if i == item("water_bucket")) == 1

# Sneaking with an item bypasses the block interaction, matching ServerPlayerGameMode.
prepare("cauldron", "water_bucket")
c.send(PLAY_SB["player_input"], b"\x20")
use(); sync(); pump(.25)
assert at() == state("cauldron") and slots[36] == (1, item("water_bucket"))
c.send(PLAY_SB["player_input"], b"\x00")

# Observe swings from a second player; first prove successful use produces one.
observer, _ = login("CauldronObserver", sys.argv[1], int(sys.argv[2]), view=2)
cmd(f"tp CauldronObserver {X + .5} {Y} {Z - 3.5}")
pump(.5)
def observed_swings(seconds=.25):
    end = time.monotonic() + seconds
    swings = 0
    while time.monotonic() < end:
        pid, b = observer.recv()
        if pid == PLAY_CB["swing_animation"]: swings += 1
    return swings
observed_swings()
use(); sync(); pump(.25)
assert observed_swings() == 1, "successful cauldron use should swing"

# Fluid above blocks lava/snow filling, including waterlogged blocks. Water buckets remain valid.
for above in ("water", "oak_slab[type=top,waterlogged=true]"):
    cmd(f"setblock {X} {Y + 1} {Z} {above}")
    for fluid in ("lava", "powder_snow"):
        prepare("cauldron", fluid + "_bucket")
        observed_swings()
        use(); sync(); pump(.25)
        assert at() == state("cauldron") and slots[36] == (1, item(fluid + "_bucket")) and not sounds, (above, fluid, at(), at(Y + 1), slots.get(36), sounds)
        assert observed_swings() == 0, "underwater rejection consumes without swinging"
    prepare("cauldron", "water_bucket")
    use()
    wait_until(lambda: at() == state("water_cauldron", level=3), "water filling underwater")
    cmd(f"setblock {X} {Y + 1} {Z} air")
    pump(.25)
observer.s.close()
cmd("gamemode creative")
cmd("clear @s")
c.s.close()
print("cauldron tests passed")
