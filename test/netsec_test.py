#!/usr/bin/env python3
"""End-to-end test for online-mode encryption + Mojang hasJoined, signed chat (ProfilePublicKey + SignedMessageChain
+ LastSeenMessages), resource-pack negotiation, server-icon.png status favicon, GS4 UDP Query, and TCP RCON."""
import base64, hashlib, http.server, json, os, shutil, socket, struct, subprocess, sys, tempfile, threading, time, uuid, zlib

sys.path.insert(0, os.path.dirname(__file__))
from bot import CFG_CB, CFG_SB, LOGIN_CB, PLAY_CB, PLAY_SB, PROTO, VERSION, Buf, free_port, skip_nbt, string, text_of, varint

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER_BIN = os.path.join(ROOT, os.environ.get("BUILD_DIR", "build"), "craftpp")

# ---------- Pure-Python AES-128 & CFB8 ----------
_SBOX = [
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
]

def _xtime(a):
    return ((a << 1) ^ 0x1B) & 0xFF if (a & 0x80) else (a << 1)

def _aes_expand_key(key):
    rk = list(key)
    rcon = 1
    for i in range(4, 44):
        t = rk[(i - 1) * 4 : i * 4]
        if i % 4 == 0:
            t = [_SBOX[t[1]] ^ rcon, _SBOX[t[2]], _SBOX[t[3]], _SBOX[t[0]]]
            rcon = _xtime(rcon)
        for j in range(4):
            rk.append(rk[(i - 4) * 4 + j] ^ t[j])
    return rk

def _aes_encrypt_block(rk, block):
    s = [block[i] ^ rk[i] for i in range(16)]
    for r in range(1, 11):
        s = [_SBOX[b] for b in s]
        s = [
            s[0], s[5], s[10], s[15],
            s[4], s[9], s[14], s[3],
            s[8], s[13], s[2], s[7],
            s[12], s[1], s[6], s[11],
        ]
        if r < 10:
            for c in range(4):
                i = c * 4
                a0, a1, a2, a3 = s[i], s[i + 1], s[i + 2], s[i + 3]
                t = a0 ^ a1 ^ a2 ^ a3
                s[i] ^= t ^ _xtime(a0 ^ a1)
                s[i + 1] ^= t ^ _xtime(a1 ^ a2)
                s[i + 2] ^= t ^ _xtime(a2 ^ a3)
                s[i + 3] ^= t ^ _xtime(a3 ^ a0)
        base = r * 16
        for i in range(16):
            s[i] ^= rk[base + i]
    return s

class AesCfb8:
    def __init__(self, key):
        self.rk = _aes_expand_key(key)
        self.enc_iv = bytearray(key)
        self.dec_iv = bytearray(key)

    def encrypt(self, data):
        out = bytearray(len(data))
        for i, b in enumerate(data):
            ks = _aes_encrypt_block(self.rk, self.enc_iv)[0]
            c = b ^ ks
            out[i] = c
            self.enc_iv = self.enc_iv[1:] + bytes([c])
        return bytes(out)

    def decrypt(self, data):
        out = bytearray(len(data))
        for i, c in enumerate(data):
            ks = _aes_encrypt_block(self.rk, self.dec_iv)[0]
            out[i] = c ^ ks
            self.dec_iv = self.dec_iv[1:] + bytes([c])
        return bytes(out)

# ---------- Pure-Python RSA / ASN.1 DER ----------
def _der_len(n):
    if n < 128:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b

def _der_int(x):
    b = x.to_bytes(max(1, (x.bit_length() + 7) // 8), "big")
    if b[0] & 0x80:
        b = b"\x00" + b
    return b"\x02" + _der_len(len(b)) + b

def spki_encode(n, e):
    seq = _der_int(n) + _der_int(e)
    rsapub = b"\x30" + _der_len(len(seq)) + seq
    bitstr = b"\x03" + _der_len(len(rsapub) + 1) + b"\x00" + rsapub
    alg = bytes([0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00])
    top = alg + bitstr
    return b"\x30" + _der_len(len(top)) + top

def _read_der_tl(buf, i):
    tag = buf[i]; i += 1
    b = buf[i]; i += 1
    if not (b & 0x80):
        return tag, b, i
    cnt = b & 0x7F
    ln = int.from_bytes(buf[i : i + cnt], "big")
    return tag, ln, i + cnt

def spki_decode(der):
    _, _, i = _read_der_tl(der, 0)  # outer SEQUENCE
    _, alen, i = _read_der_tl(der, i)  # AlgorithmIdentifier
    i += alen
    _, _, i = _read_der_tl(der, i)  # BIT STRING
    i += 1  # unused bits = 0
    _, _, i = _read_der_tl(der, i)  # RSAPublicKey SEQUENCE
    _, nlen, i = _read_der_tl(der, i)
    n = int.from_bytes(der[i : i + nlen], "big"); i += nlen
    _, elen, i = _read_der_tl(der, i)
    e = int.from_bytes(der[i : i + elen], "big")
    return n, e

def rsa_encrypt_pkcs1(n, e, msg):
    k = (n.bit_length() + 7) // 8
    pad_len = k - 3 - len(msg)
    pad = bytes(b or 1 for b in os.urandom(pad_len))
    em = b"\x00\x02" + pad + b"\x00" + msg
    c = pow(int.from_bytes(em, "big"), e, n)
    return c.to_bytes(k, "big")

def rsa_sign_pkcs1(n, d, hash_name, data):
    k = (n.bit_length() + 7) // 8
    if hash_name == "sha1":
        digest = hashlib.sha1(data).digest()
        prefix = bytes([0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14])
    else:
        digest = hashlib.sha256(data).digest()
        prefix = bytes([0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20])
    t = prefix + digest
    em = b"\x00\x01" + (b"\xff" * (k - 3 - len(t))) + b"\x00" + t
    s = pow(int.from_bytes(em, "big"), d, n)
    return s.to_bytes(k, "big")

def _is_probable_prime(n):
    if n < 2 or n % 2 == 0:
        return n == 2
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2; r += 1
    for a in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29):
        x = pow(a, d, n)
        if x == 1 or x == n - 1:
            continue
        for _ in range(r - 1):
            x = pow(x, 2, n)
            if x == n - 1:
                break
        else:
            return False
    return True

def _gen_prime(bits):
    while True:
        p = int.from_bytes(os.urandom(bits // 8), "big") | (1 << (bits - 1)) | 1
        if _is_probable_prime(p):
            return p

def gen_rsa_keypair(bits=2048):
    e = 65537
    while True:
        p = _gen_prime(bits // 2)
        q = _gen_prime(bits // 2)
        if p != q and (p - 1) % e != 0 and (q - 1) % e != 0 and (p * q).bit_length() == bits:
            n = p * q
            phi = (p - 1) * (q - 1)
            d = pow(e, -1, phi)
            return n, e, d, spki_encode(n, e)

def mc_server_id_hex(secret, pub_der):
    digest = hashlib.sha1(b"" + secret + pub_der).digest()
    val = int.from_bytes(digest, "big", signed=True)
    return f"-{-val:x}" if val < 0 else f"{val:x}"

def make_png_64x64():
    ihdr = struct.pack(">IIBBBBB", 64, 64, 8, 2, 0, 0, 0)
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + b"\x20\x60\xa0" * 64 for _ in range(64))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")

# ---------- Encrypted Client Connection ----------
class SecureConn:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=15)
        self.cipher = None
        self.threshold = -1
        self.play = False
        self.pos = (0.0, 0.0, 0.0)

    def _read_exact(self, n):
        out = bytearray()
        while len(out) < n:
            chunk = self.s.recv(n - len(out))
            assert chunk, "server closed connection"
            out.extend(chunk)
        data = bytes(out)
        return self.cipher.decrypt(data) if self.cipher else data

    def send(self, pid, body=b""):
        p = varint(pid) + body
        if self.threshold >= 0:
            p = varint(len(p)) + zlib.compress(p, 1) if len(p) >= self.threshold else b"\x00" + p
        raw = varint(len(p)) + p
        if self.cipher:
            raw = self.cipher.encrypt(raw)
        self.s.sendall(raw)

    def recv(self):
        n = s = 0
        while True:
            b = self._read_exact(1)[0]
            n |= (b & 0x7F) << s; s += 7
            if not (b & 0x80):
                break
        b = Buf(self._read_exact(n))
        if self.threshold >= 0 and b.varint():
            b = Buf(zlib.decompress(b.d[b.i:]))
        pid = b.varint()
        cb, sb = (PLAY_CB, PLAY_SB) if self.play else (CFG_CB, CFG_SB)
        if pid == cb.get("keep_alive", -1):
            self.send(sb["keep_alive"], b.d[b.i : b.i + 8])
        elif self.play and pid == PLAY_CB["chunk_batch_finished"]:
            self.send(PLAY_SB["chunk_batch_received"], struct.pack(">f", 50.0))
        elif self.play and pid == PLAY_CB["player_position"]:
            d = b.d[b.i:]
            tid = Buf(d).varint(); ln = len(varint(tid))
            self.pos = struct.unpack(">ddd", d[ln : ln + 24])
            self.send(PLAY_SB["accept_teleportation"], d[: ln + 24] + d[ln + 48 : ln + 56])
        return pid, b

def wait_pkt(c, pid, pred=lambda b: True, limit=50000):
    for _ in range(limit):
        p, b = c.recv()
        if p == pid and pred(b):
            return b
    raise AssertionError(f"packet {pid} not received")

def online_login(name, uid, host, port, session_store, decline_pack=False):
    c = SecureConn(host, port)
    c.send(0, varint(PROTO) + string(host) + struct.pack(">H", port) + varint(2))
    c.send(0, string(name) + uid.bytes)
    pid, b = c.recv()
    assert pid == LOGIN_CB["hello"], f"expected LOGIN_CB.hello, got {pid}"
    srv_id = b.str()
    pub_der = b.take(b.varint())
    challenge = b.take(b.varint())
    should_auth = b.u8()
    assert srv_id == "" and len(challenge) == 4 and should_auth == 1

    srv_n, srv_e = spki_decode(pub_der)
    secret = os.urandom(16)
    expected_hash = mc_server_id_hex(secret, pub_der)
    if session_store is not None:
        session_store[(name, expected_hash)] = {
            "id": uid.hex,
            "name": name,
            "properties": [{"name": "textures", "value": "dGVzdA==", "signature": "c2ln"}],
        }
    enc_secret = rsa_encrypt_pkcs1(srv_n, srv_e, secret)
    enc_chal = rsa_encrypt_pkcs1(srv_n, srv_e, challenge)
    c.send(1, varint(len(enc_secret)) + enc_secret + varint(len(enc_chal)) + enc_chal)
    c.cipher = AesCfb8(secret)

    pid, b = c.recv()
    if pid == LOGIN_CB["login_disconnect"]:
        return c, ("login_disconnect", b.str())
    if pid == LOGIN_CB["login_compression"]:
        c.threshold = b.varint()
        pid, b = c.recv()
    assert pid == LOGIN_CB["login_finished"], (pid, b.d[b.i:])
    got_uid = uuid.UUID(bytes=b.take(16))
    got_name = b.str()
    prop_count = b.varint()
    props_out = []
    for _ in range(prop_count):
        pn, pv = b.str(), b.str()
        ps = b.str() if b.u8() else ""
        props_out.append((pn, pv, ps))
    assert got_uid == uid and got_name == name and props_out == [("textures", "dGVzdA==", "c2ln")], (got_uid, got_name, props_out)

    c.send(3)  # login_acknowledged
    c.send(CFG_SB["client_information"], string("en_us") + bytes([2]) + varint(0) + b"\x01\x7f" + varint(1) + b"\x00\x01" + varint(0))
    rp_seen = None
    while True:
        pid, b = c.recv()
        if pid == CFG_CB["select_known_packs"]:
            c.send(CFG_SB["select_known_packs"], varint(1) + string("minecraft") + string("core") + string(VERSION))
        elif pid == CFG_CB["resource_pack_push"]:
            rp_uid = uuid.UUID(bytes=b.take(16))
            rp_url = b.str()
            rp_hash = b.str()
            rp_req = b.u8()
            rp_prompt = text_of(b) if b.u8() else ""
            rp_seen = (rp_uid, rp_url, rp_hash, rp_req, rp_prompt)
            if decline_pack:
                c.send(CFG_SB["resource_pack"], rp_uid.bytes + varint(1))  # DECLINED
            else:
                c.send(CFG_SB["resource_pack"], rp_uid.bytes + varint(3))  # ACCEPTED
                c.send(CFG_SB["resource_pack"], rp_uid.bytes + varint(0))  # SUCCESSFULLY_LOADED
        elif pid == CFG_CB["disconnect"]:
            return c, ("config_disconnect", b.d[b.i:], rp_seen)
        elif pid == CFG_CB["finish_configuration"]:
            c.send(CFG_SB["finish_configuration"])
            break

    c.play = True
    login_flags = None
    while True:
        pid, b = c.recv()
        if pid == PLAY_CB["login"]:
            b.i32(); b.u8()
            for _ in range(b.varint()):
                b.str()
            b.varint(); b.varint(); b.varint(); b.take(3)
            # CommonPlayerSpawnInfo: dimensionType(varint), dimension(str), seed(i64), gamemode(u8), prev_gamemode(i8), isDebug(u8), isFlat(u8), hasDeathLoc(u8), portalCooldown(varint), seaLevel(varint)
            b.varint(); b.str(); b.i64(); b.u8(); b.u8(); b.u8(); b.u8()
            if b.u8():
                b.str(); b.i64()
            b.varint(); b.varint()
            login_flags = (b.u8(), b.u8())
        elif pid == PLAY_CB["player_position"]:
            break
    c.send(PLAY_SB["player_loaded"])
    return c, ("ok", rp_seen, login_flags)

def java_byte_array_hash(bs):
    h = 1
    for b in bs:
        sb = b - 256 if b >= 128 else b
        h = ((31 * h) + sb) & 0xFFFFFFFF
    return h - 0x100000000 if h >= 0x80000000 else h

def last_seen_checksum(sigs):
    c = 1
    for s in sigs:
        c = ((31 * c) + java_byte_array_hash(s)) & 0xFFFFFFFF
    b = c & 0xFF
    sb = b - 256 if b >= 128 else b
    return 1 if sb == 0 else sb

def sign_chat_message(priv_n, priv_d, sender_uid, session_uid, index, salt, timestamp_ms, content, last_seen_sigs):
    body = bytearray()
    body += struct.pack(">I", 1)
    body += sender_uid.bytes
    body += session_uid.bytes
    body += struct.pack(">IqqI", index, salt, timestamp_ms // 1000, len(content.encode()))
    body += content.encode()
    body += struct.pack(">I", len(last_seen_sigs))
    for s in last_seen_sigs:
        body += s
    return rsa_sign_pkcs1(priv_n, priv_d, "sha256", bytes(body))

def main():
    sessions = {}

    class MockSessionHandler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            from urllib.parse import parse_qs, urlparse
            q = parse_qs(urlparse(self.path).query)
            user = q.get("username", [""])[0]
            sid = q.get("serverId", [""])[0]
            prof = sessions.get((user, sid))
            if prof:
                body = json.dumps(prof).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self.send_response(204)
                self.end_headers()
        def log_message(self, *args):
            pass

    httpd = http.server.HTTPServer(("127.0.0.1", 0), MockSessionHandler)
    http_port = httpd.server_address[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    srv_n, srv_e, srv_d, srv_spki = gen_rsa_keypair(2048)
    srv_spki_b64 = base64.b64encode(srv_spki).decode()

    d = tempfile.mkdtemp(prefix="craftpp-netsec-")
    port = free_port()
    qport = free_port()
    rport = free_port()

    alice_uid = uuid.UUID("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee")
    bob_uid = uuid.UUID("bbbbbbbb-cccc-4ddd-8eee-ffffffffffff")

    open(os.path.join(d, "eula.txt"), "w").write("eula=true\n")
    open(os.path.join(d, "server-icon.png"), "wb").write(make_png_64x64())
    open(os.path.join(d, "ops.json"), "w").write(json.dumps([{"uuid": str(alice_uid), "name": "Alice", "level": 4}]))
    props_path = os.path.join(d, "server.properties")
    open(props_path, "w").write("\n".join([
        f"server-port={port}",
        f"level-name={d}/world",
        "level-type=flat",
        "gamemode=creative",
        "view-distance=2",
        "online-mode=true",
        "white-list=false",
        "enforce-secure-profile=true",
        f"session-server-url=http://127.0.0.1:{http_port}/session/minecraft/hasJoined",
        f"services-public-key-der-base64={srv_spki_b64}",
        "resource-pack=https://example.com/craftpp-pack.zip",
        "resource-pack-sha1=0123456789abcdef0123456789abcdef01234567",
        "require-resource-pack=true",
        "resource-pack-prompt=Required Pack Prompt",
        "enable-query=true",
        f"query.port={qport}",
        "enable-rcon=true",
        f"rcon.port={rport}",
        "rcon.password=secret123",
        "motd=NetSec Test Server",
        "",
    ]))

    log_path = os.path.join(d, "server.log")
    log_f = open(log_path, "w")
    proc = subprocess.Popen([SERVER_BIN, props_path], stdin=subprocess.PIPE, stdout=log_f, stderr=subprocess.STDOUT, text=True)
    try:
        for _ in range(300):
            if "Done (" in open(log_path).read():
                break
            time.sleep(0.05)
        assert "Done (" in open(log_path).read(), open(log_path).read()

        # 1. Status ping: favicon + enforcesSecureChat
        s = socket.create_connection(("127.0.0.1", port), timeout=10)
        hs = varint(0) + varint(PROTO) + string("127.0.0.1") + struct.pack(">H", port) + varint(1)
        s.sendall(varint(len(hs)) + hs + b"\x01\x00")
        f = s.makefile("rb")
        n = sh = 0
        while True:
            b = f.read(1)[0]; n |= (b & 0x7F) << sh; sh += 7
            if not (b & 0x80): break
        sb = Buf(f.read(n))
        assert sb.varint() == 0
        status = json.loads(sb.str())
        s.close()
        assert status.get("enforcesSecureChat") is True, status
        assert status.get("favicon", "").startswith("data:image/png;base64,"), status
        print("status favicon + enforcesSecureChat ok")

        # 2. Online-mode auth failure (no session in MockSessionHandler)
        bad_c, res = online_login("Hacker", uuid.uuid4(), "127.0.0.1", port, None)
        bad_c.s.close()
        assert res[0] == "login_disconnect" and "Failed to verify username!" in res[1], res
        print("online-mode invalid session rejection ok")

        # 3. Required resource pack rejection
        dec_c, res = online_login("Decliner", uuid.uuid4(), "127.0.0.1", port, sessions, decline_pack=True)
        dec_c.s.close()
        assert res[0] == "config_disconnect" and b"multiplayer.requiredTexturePrompt.disconnect" in res[1], res
        print("required resource pack decline disconnect ok")

        # 4. Online-mode login + AES-128/CFB8 + resource pack accept for Alice and Bob
        alice, a_res = online_login("Alice", alice_uid, "127.0.0.1", port, sessions)
        assert a_res[0] == "ok" and a_res[2] == (1, 1), a_res
        rp_uid, rp_url, rp_hash, rp_req, rp_prompt = a_res[1]
        assert rp_url == "https://example.com/craftpp-pack.zip" and rp_req == 1 and rp_prompt == "Required Pack Prompt"

        bob, b_res = online_login("Bob", bob_uid, "127.0.0.1", port, sessions)
        assert b_res[0] == "ok"
        print("online-mode RSA/AES-128-CFB8 login + resource pack accept ok")

        # 5. Unsigned chat rejected when enforce-secure-profile=true
        alice.send(PLAY_SB["chat"], string("unsigned test") + struct.pack(">qq", int(time.time() * 1000), 1) + b"\x00" + varint(0) + bytes(3) + b"\x00")
        err_pkt = wait_pkt(alice, PLAY_CB["system_chat"], lambda b: b"chat.disabled.missingProfileKey" in b.d)
        assert err_pkt is not None

        # 6. Signed chat session update + SHA256withRSA message chain + LastSeenMessages
        a_n, a_e, a_d, a_spki = gen_rsa_keypair(2048)
        b_n, b_e, b_d, b_spki = gen_rsa_keypair(2048)
        exp_ms = int((time.time() + 3600) * 1000)
        a_key_sig = rsa_sign_pkcs1(srv_n, srv_d, "sha1", alice_uid.bytes + struct.pack(">q", exp_ms) + a_spki)
        b_key_sig = rsa_sign_pkcs1(srv_n, srv_d, "sha1", bob_uid.bytes + struct.pack(">q", exp_ms) + b_spki)
        a_sess = uuid.uuid4()
        b_sess = uuid.uuid4()

        alice.send(PLAY_SB["chat_session_update"], a_sess.bytes + struct.pack(">q", exp_ms) + varint(len(a_spki)) + a_spki + varint(len(a_key_sig)) + a_key_sig)
        bob.send(PLAY_SB["chat_session_update"], b_sess.bytes + struct.pack(">q", exp_ms) + varint(len(b_spki)) + b_spki + varint(len(b_key_sig)) + b_key_sig)

        # Wait for Bob to receive Alice's INITIALIZE_CHAT (action 0x02) in player_info_update
        wait_pkt(bob, PLAY_CB["player_info_update"], lambda b: b.d[b.i] == 0x02 and b.d[b.i + 2 : b.i + 18] == alice_uid.bytes)

        ts1 = int(time.time() * 1000)
        salt1 = 0x1122334455667788
        msg1 = "Hello secure world!"
        sig1 = sign_chat_message(a_n, a_d, alice_uid, a_sess, 0, salt1, ts1, msg1, [])
        alice.send(PLAY_SB["chat"], string(msg1) + struct.pack(">qq", ts1, salt1) + b"\x01" + sig1 + varint(0) + bytes(3) + bytes([1]))

        pc = wait_pkt(bob, PLAY_CB["player_chat"])
        pc.varint()  # global_index
        assert uuid.UUID(bytes=pc.take(16)) == alice_uid
        assert pc.varint() == 0  # link index
        assert pc.u8() == 1  # has signature
        got_sig1 = pc.take(256)
        assert got_sig1 == sig1 and pc.str() == msg1

        # Bob acknowledges sig1 in LastSeenMessages (offset 1, bit 19 set) and replies
        ts2 = ts1 + 1000
        salt2 = 0x2233445566778899
        msg2 = "Signed reply with last-seen!"
        csum = last_seen_checksum([sig1]) & 0xFF
        sig2 = sign_chat_message(b_n, b_d, bob_uid, b_sess, 0, salt2, ts2, msg2, [sig1])
        bob.send(PLAY_SB["chat"], string(msg2) + struct.pack(">qq", ts2, salt2) + b"\x01" + sig2 + varint(1) + b"\x00\x00\x08" + bytes([csum]))

        pc2 = wait_pkt(alice, PLAY_CB["player_chat"], lambda b: b"Signed reply with last-seen!" in b.d)
        pc2.varint()
        assert uuid.UUID(bytes=pc2.take(16)) == bob_uid
        assert pc2.varint() == 0 and pc2.u8() == 1 and pc2.take(256) == sig2 and pc2.str() == msg2
        pc2.take(16)  # timestamp + salt
        assert pc2.varint() == 1 and pc2.varint() == 0 and pc2.take(256) == sig1
        print("signed chat session + SHA256withRSA message chain + LastSeenMessages ok")

        # 7. GS4 UDP Query (handshake + basic stat + full stat)
        us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        us.settimeout(5)
        us.sendto(b"\xfe\xfd\x09\x12\x34\x56\x78", ("127.0.0.1", qport))
        qresp, _ = us.recvfrom(1460)
        assert qresp[:5] == b"\x09\x12\x34\x56\x78"
        tok = int(qresp[5:-1].decode())
        # Basic stat
        us.sendto(b"\xfe\xfd\x00\x12\x34\x56\x78" + struct.pack(">i", tok), ("127.0.0.1", qport))
        bstat, _ = us.recvfrom(1460)
        assert bstat[:5] == b"\x00\x12\x34\x56\x78" and b"NetSec Test Server\x00SMP\x00" in bstat
        # Full stat
        us.sendto(b"\xfe\xfd\x00\x12\x34\x56\x78" + struct.pack(">i", tok) + b"\x00\x00\x00\x00", ("127.0.0.1", qport))
        fstat, _ = us.recvfrom(1460)
        assert b"hostname\x00NetSec Test Server\x00" in fstat and b"Alice\x00" in fstat and b"Bob\x00" in fstat
        us.close()
        print("GS4 UDP Query (handshake + basic + full stat) ok")

        # 8. TCP RCON (auth failure, auth success, command execution)
        rs = socket.create_connection(("127.0.0.1", rport), timeout=5)
        def rcon_call(req_id, rtype, payload):
            b = payload.encode() + b"\x00\x00"
            rs.sendall(struct.pack("<iii", len(b) + 8, req_id, rtype) + b)
            hdr = rs.recv(4)
            ln = struct.unpack("<i", hdr)[0]
            data = rs.recv(ln)
            rid, rt = struct.unpack("<ii", data[:8])
            return rid, rt, data[8:-2].decode()

        rid, rt, _ = rcon_call(10, 3, "wrongpass")
        assert rid == -1, rid
        rid, rt, _ = rcon_call(11, 3, "secret123")
        assert rid == 11 and rt == 2, (rid, rt)
        rid, rt, out = rcon_call(12, 2, "list")
        assert rid == 12 and "There are 2 of a max of 20 players online" in out and "Alice" in out and "Bob" in out, out
        rs.close()
        print("RCON auth + command execution ok:", out)

        alice.s.close()
        bob.s.close()
    except Exception as e:
        log_f.flush()
        print("TEST FAILED WITH EXCEPTION:", e)
        print("SERVER LOG TAIL:")
        with open(log_path) as lf:
            print(lf.read()[-3000:])
        raise
    finally:
        try:
            proc.stdin.write("stop\n")
            proc.stdin.flush()
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
        log_f.close()
        httpd.server_close()
        shutil.rmtree(d, ignore_errors=True)
    print("netsec tests passed")

if __name__ == "__main__":
    main()
