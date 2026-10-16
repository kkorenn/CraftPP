#!/usr/bin/env python3
"""Server list ping check: python3 test/ping.py [host] [port]"""
import json, socket, struct, sys, time

def varint(n):
    out = b""
    while True:
        b = n & 0x7F
        n >>= 7
        out += bytes([b | (0x80 if n else 0)])
        if not n:
            return out

def read_varint(s):
    n = shift = 0
    while True:
        b = s.recv(1)[0]
        n |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return n

def packet(body):
    return varint(len(body)) + body

def recv_packet(s):
    n = read_varint(s)
    data = b""
    while len(data) < n:
        data += s.recv(n - len(data))
    return data

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 25565
s = socket.create_connection((host, port))
addr = host.encode()
s.sendall(packet(varint(0) + varint(777) + varint(len(addr)) + addr + struct.pack(">H", port) + varint(1)))
s.sendall(packet(varint(0)))
data = recv_packet(s)
assert data[0] == 0
i, ln, shift = 1, 0, 0
while True:
    b = data[i]; i += 1
    ln |= (b & 0x7F) << shift; shift += 7
    if not b & 0x80:
        break
status = json.loads(data[i:i + ln])
assert status["version"]["protocol"] == 777, status
t = int(time.time() * 1000)
s.sendall(packet(varint(1) + struct.pack(">q", t)))
pong = recv_packet(s)
assert pong == varint(1) + struct.pack(">q", t), pong
print("OK", json.dumps(status))
