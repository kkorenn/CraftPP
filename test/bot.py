#!/usr/bin/env python3
"""Headless join check: login -> configuration -> play, parses every chunk.
Usage: python3 test/bot.py [host] [port]"""
import re, socket, struct, sys, uuid, zlib

HDR = open(__file__.rsplit("/", 2)[0] + "/src/gen/ids.hpp").read()
PROTO = int(re.search(r"PROTOCOL_VERSION = (\d+)", HDR).group(1))
VERSION = re.search(r'GAME_VERSION = "([^"]+)"', HDR).group(1)

def ids(ns):
    body = re.search(r"namespace %s \{(.*?)\}" % ns, HDR, re.S).group(1)
    return {k: int(v) for k, v in re.findall(r"int (\w+) = (\d+);", body)}

LOGIN_CB, CFG_CB, CFG_SB, PLAY_CB, PLAY_SB = map(ids, ["login_cb", "config_cb", "config_sb", "play_cb", "play_sb"])

def free_port():
    """A free port below every OS's ephemeral range (Linux 32768+, macOS 49152+): outgoing connections and bind(0)
    probes from parallel test runs never take it while a test server restarts on it."""
    import random
    while True:
        s, port = socket.socket(), random.randint(20000, 32767)
        try:
            s.bind(("", port))
            return port
        except OSError:
            pass
        finally:
            s.close()

def varint(n):
    n &= 0xFFFFFFFF
    out = b""
    while True:
        b = n & 0x7F; n >>= 7
        out += bytes([b | (0x80 if n else 0)])
        if not n: return out

def string(s): return varint(len(s.encode())) + s.encode()

class Buf:
    def __init__(self, d): self.d, self.i = d, 0
    def take(self, n):
        assert self.i + n <= len(self.d), "read past end"
        r = self.d[self.i:self.i + n]; self.i += n; return r
    def u8(self): return self.take(1)[0]
    def varint(self):
        n = s = 0
        while True:
            b = self.u8(); n |= (b & 0x7F) << s; s += 7
            if not b & 0x80: return n
    def str(self): return self.take(self.varint()).decode()
    def i32(self): return struct.unpack(">i", self.take(4))[0]
    def i64(self): return struct.unpack(">q", self.take(8))[0]
    def i16(self): return struct.unpack(">h", self.take(2))[0]
    def u16(self): return struct.unpack(">H", self.take(2))[0]

class Conn:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port))
        self.f = self.s.makefile("rb")
        self.play, self.biomes, self.threshold = False, [], -1
    def send(self, pid, body=b""):
        p = varint(pid) + body
        if self.threshold >= 0:  # set_compression received
            p = varint(len(p)) + zlib.compress(p, 1) if len(p) >= self.threshold else b"\x00" + p
        self.s.sendall(varint(len(p)) + p)
    def recv(self):
        n = s = 0
        while True:
            b = self.f.read(1)
            assert b, "server closed connection"
            n |= (b[0] & 0x7F) << s; s += 7
            if not b[0] & 0x80: break
        b = Buf(self.f.read(n))
        if self.threshold >= 0 and b.varint():
            b = Buf(zlib.decompress(b.d[b.i:]))
        pid = b.varint()
        # Housekeeping vanilla insists on: keep-alives and chunk batch acks (it throttles without them).
        cb, sb = (PLAY_CB, PLAY_SB) if self.play else (CFG_CB, CFG_SB)
        if pid == cb["keep_alive"]: self.send(sb["keep_alive"], b.d[b.i:b.i + 8])
        elif self.play and pid == PLAY_CB["chunk_batch_finished"]: self.send(PLAY_SB["chunk_batch_received"], struct.pack(">f", 50.0))
        elif self.play and pid == PLAY_CB["player_position"]:  # vanilla ignores movement until teleports are accepted
            d = b.d[b.i:]
            tid = Buf(d).varint(); n = len(varint(tid))
            self.pos = struct.unpack(">ddd", d[n:n + 24])
            self.send(PLAY_SB["accept_teleportation"], d[:n + 24] + d[n + 48:n + 56])
        return pid, b
    def close(self):
        try: self.f.close()
        except Exception: pass
        try: self.s.close()
        except Exception: pass

def skip_nbt(b, t=None):
    """Skip a network NBT tag (nameless root when t is None)."""
    if t is None:
        t = b.u8()
        if t == 0: return
    if t in (1, 2, 3, 4, 5, 6): b.take([1, 2, 4, 8, 4, 8][t - 1])
    elif t == 7: b.take(b.i32())
    elif t == 8: b.take(b.u16())
    elif t == 9:
        et, n = b.u8(), b.i32()
        for _ in range(n): skip_nbt(b, et)
    elif t == 10:
        while (et := b.u8()): b.take(b.u16()); skip_nbt(b, et)
    elif t in (11, 12): b.take((4 if t == 11 else 8) * b.i32())
    else: raise AssertionError(f"bad nbt tag {t}")

def paletted(b, entries, max_bits, decode=False):
    """Read a paletted container; with decode, return the list of global ids."""
    bits = b.u8()
    if bits == 0:
        v = b.varint(); return [v] * entries if decode else None
    pal = [b.varint() for _ in range(b.varint())] if bits <= max_bits else None
    per = 64 // bits
    raw = b.take(8 * ((entries + per - 1) // per))
    if not decode: return None
    mask, out = (1 << bits) - 1, []
    for i in range(0, len(raw), 8):
        v = int.from_bytes(raw[i:i + 8], "big")
        out += [(v >> (j * bits)) & mask for j in range(per)]
    del out[entries:]
    return [pal[x] for x in out] if pal else out

def parse_chunk(b, decode=False):
    """Returns (cx, cz, sections); with decode, sections = [(4096 block ids (y,z,x), 64 biome ids)]."""
    cx, cz = b.i32(), b.i32()
    for _ in range(b.varint()):
        b.varint(); b.take(8 * b.varint())
    data = Buf(b.take(b.varint()))
    sections = []
    while data.i < len(data.d):
        data.i16(); data.i16()
        sections.append((paletted(data, 4096, 8, decode), paletted(data, 64, 3, decode)))
    assert len(sections) in (16, 24), len(sections)  # nether/end 0..255, overworld -64..319
    for _ in range(b.varint()):  # block entities
        b.u8(); b.i16(); b.varint(); skip_nbt(b)
    light = parse_light(b)
    assert b.i == len(b.d), "trailing bytes in chunk"
    return (cx, cz, sections, light) if decode else (cx, cz, sections)

def parse_light(b):
    """Light data (chunk packet tail / light_update): {"sky"|"block": {section index 0..25: 2048 bytes}}.
    Sections in the empty mask come back as zeros; sections in neither mask are absent."""
    masks = [int.from_bytes(b.take(b.varint()), "little") for _ in range(4)]  # BitSet as LE byte array
    out = {}
    for k, (mask, empty) in zip(("sky", "block"), ((masks[0], masks[2]), (masks[1], masks[3]))):
        layers = [b.take(b.varint()) for _ in range(b.varint())]
        assert all(len(l) == 2048 for l in layers) and len(layers) == bin(mask).count("1")
        d = out[k] = {}
        for i in range(26):
            if mask >> i & 1: d[i] = layers.pop(0)
            elif empty >> i & 1: d[i] = bytes(2048)
    return out

def login(name, host, port, view=8, decode=False, uid=None):
    """Join and stop at the first teleport. c.chunks holds chunks seen so far, c.spawn the world spawn."""
    c = Conn(host, port)
    c.chunks, c.light, c.spawn, c.registries = {}, {}, None, 0
    c.send(0, varint(PROTO) + string(host) + struct.pack(">H", port) + varint(2))
    c.send(0, string(name) + (uid or uuid.uuid4()).bytes)
    pid, b = c.recv()
    if pid == LOGIN_CB["login_compression"]:
        c.threshold = b.varint()
        pid, b = c.recv()
    assert pid == LOGIN_CB["login_finished"], (pid, b.d[b.i:])
    c.send(3)  # login acknowledged
    c.send(CFG_SB["client_information"], string("en_us") + bytes([view]) + varint(0) + b"\x01\x7f" + varint(1) + b"\x00\x01" + varint(0))
    while True:
        pid, b = c.recv()
        if pid == CFG_CB["select_known_packs"]:
            c.send(CFG_SB["select_known_packs"], varint(1) + string("minecraft") + string("core") + string(VERSION))
        elif pid == CFG_CB["registry_data"]:
            c.registries += 1
            reg, names = b.str(), []
            for _ in range(b.varint()):
                names.append(b.str())
                if b.u8(): skip_nbt(b)
            if reg == "minecraft:worldgen/biome": c.biomes = names
        elif pid == CFG_CB["code_of_conduct"]:
            c.send(CFG_SB["accept_code_of_conduct"])
        elif pid == CFG_CB["finish_configuration"]:
            c.send(CFG_SB["finish_configuration"]); break
    c.play = True
    c.decode = decode
    while play_step(c) != PLAY_CB["player_position"]: pass
    c.send(PLAY_SB["player_loaded"])
    return c, len(c.chunks)

def walk(c, x, y, z, ground=True, step=4.0):
    """Move in steps of at most `step` blocks a tick, like a real client (vanilla rejects >10 blocks a tick)."""
    import time
    sx, sy, sz = getattr(c, "pos", (x, y, z))
    n = max(1, int(max(abs(x - sx), abs(y - sy), abs(z - sz)) // step) + 1)
    for i in range(1, n + 1):
        f = i / n
        last = i == n
        c.send(PLAY_SB["move_player_pos"], struct.pack(">dddB", sx + (x - sx) * f, sy + (y - sy) * f, sz + (z - sz) * f,
                                                         1 if ground and last else 0))
        if not last: time.sleep(0.06)
    c.pos = (x, y, z)

def sext(v, n): return (v & (1 << n) - 1 ^ 1 << n - 1) - (1 << n - 1)
def block_pos(v): return v >> 38, sext(v, 12), sext(v >> 12, 26)  # packed BlockPos x:26 z:26 y:12

def set_block(c, x, y, z, state):
    if (x >> 4, z >> 4) in c.chunks:
        secs = c.chunks[x >> 4, z >> 4]
        min_y = -64 if len(secs) == 24 else 0
        if min_y <= y < min_y + 16 * len(secs):
            secs[(y - min_y) >> 4][0][((y & 15) * 16 + (z & 15)) * 16 + (x & 15)] = state

def play_step(c):
    """Receive one play packet, recording chunks (c.chunks) and world spawn (c.spawn), acking teleports."""
    pid, b = c.recv()
    c.last = b
    if pid == PLAY_CB["level_chunk_with_light"]:
        if c.decode:
            cx, cz, secs, c.light[cx, cz] = parse_chunk(b, True)
        else:
            cx, cz, secs = parse_chunk(b)
        c.chunks[cx, cz] = secs
    elif pid == PLAY_CB["light_update"] and c.decode:
        cx, cz = b.varint(), b.varint()
        for k, d in parse_light(b).items(): c.light.setdefault((cx, cz), {"sky": {}, "block": {}})[k].update(d)
    elif pid == PLAY_CB["login"]:
        b.i32(); b.u8(); [b.str() for _ in range(b.varint())]; b.varint(); b.varint(); b.varint(); b.take(3)
        b.varint(); c.dimension = b.str()
    elif pid == PLAY_CB["respawn"]:  # new level: the client drops every chunk
        b.varint(); c.dimension = b.str(); c.chunks.clear()
    elif pid == PLAY_CB["set_default_spawn_position"]:
        b.str(); c.spawn = block_pos(b.i64())
    elif pid == PLAY_CB["block_update"] and c.decode:
        set_block(c, *block_pos(b.i64()), b.varint())
    elif pid == PLAY_CB["section_blocks_update"] and c.decode:
        v = b.i64(); sx, sy, sz = v >> 42, sext(v, 20), sext(v >> 20, 22)
        for _ in range(b.varint()):
            e = b.varint()
            set_block(c, sx * 16 + (e >> 8 & 15), sy * 16 + (e & 15), sz * 16 + (e >> 4 & 15), e >> 12)
    return pid  # (teleports are accepted in Conn.recv)

def text_of(b):
    """Pull the 'text' string out of an NBT text component."""
    d = b.d[b.i:]
    i = d.index(b"\x08\x00\x04text") + 7
    n = struct.unpack(">H", d[i:i + 2])[0]
    return d[i + 2:i + 2 + n].decode()

def wait(c, pid, pred=lambda b: True, limit=50000):  # mobs flood entity packets
    for _ in range(limit):
        p, b = c.recv()
        if p == pid and pred(b): return b
    raise AssertionError(f"packet {pid} not received")

if __name__ == "__main__":
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 25565

    a, chunks = login("Alice", host, port)
    assert a.registries == 32, a.registries
    assert chunks == 289, chunks
    print(f"join ok: {chunks} chunks")

    b, _ = login("Bob", host, port)
    wait(b, PLAY_CB["add_entity"])  # Bob sees Alice
    wait(a, PLAY_CB["add_entity"])  # Alice sees Bob
    print("visibility ok")

    # Alice walks one chunk east: expect 17 new chunks + 17 forgets, Bob sees her move.
    walk(a, 16.5, -60, 0.5)
    new = forgot = 0
    while new < 17 or forgot < 17:
        p, buf = a.recv()
        if p == PLAY_CB["level_chunk_with_light"]: parse_chunk(buf); new += 1
        elif p == PLAY_CB["forget_level_chunk"]: forgot += 1
    assert (new, forgot) == (17, 17), (new, forgot)
    seen_x = [0.5]  # Bob tracks Alice from her spawn x: relative moves (1/4096 block) or position syncs
    def follow(pkt):
        p, x = pkt
        if p in (PLAY_CB["move_entity_pos"], PLAY_CB["move_entity_pos_rot"]):
            x.varint(); x.varint(); seen_x[0] += struct.unpack(">h", x.take(2))[0] / 4096.0
        elif p == PLAY_CB["entity_position_sync"]:
            x.varint(); x.varint(); seen_x[0] = struct.unpack(">d", x.take(8))[0]
        return abs(seen_x[0] - 16.5) < 1e-3
    for _ in range(4000):
        if follow(b.recv()): break
    assert abs(seen_x[0] - 16.5) < 1e-3, seen_x
    print("streaming + movement ok")

    a.send(PLAY_SB["chat"], string("hello") + struct.pack(">qq", 0, 0) + b"\x00" + varint(0) + bytes(3) + b"\x00")
    pc = wait(b, PLAY_CB["player_chat"])  # unsigned player chat, chat type minecraft:chat
    pc.varint(); pc.take(16); pc.varint()
    assert pc.u8() == 0 and pc.str() == "hello"
    pc.take(16); assert pc.varint() == 0 and pc.u8() == 0 and pc.varint() == 0 and pc.varint() == 1
    assert text_of(pc) == "Alice"
    a.send(PLAY_SB["chat_command"], string("list"))
    lst = wait(a, PLAY_CB["system_chat"], lambda x: b"commands.list.players" in x.d).d  # translatable, like vanilla
    assert b"Alice" in lst and b"Bob" in lst, lst
    msg = "list ok"
    a.send(PLAY_SB["chat_command"], string("gamemode survival"))
    ev = wait(a, PLAY_CB["game_event"], lambda x: x.d[x.i] == 3)
    print("chat + commands ok:", msg)

    b.s.shutdown(socket.SHUT_RDWR)
    wait(a, PLAY_CB["remove_entities"])
    print("leave ok")
