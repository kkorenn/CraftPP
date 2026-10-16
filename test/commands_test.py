#!/usr/bin/env python3
"""Command system checks: permission-filtered command tree, errors, world clock persistence, block edits,
scoreboard + execute, suggestions. Usage: python3 test/commands_test.py host port [--after-restart]
Expects build/ops.json to op "Alice" (build.sh writes it)."""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from bot import login, wait, varint, string, Buf, PLAY_CB, PLAY_SB, ids, play_step

ARG = ids("arg_types")
STATE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "commands_test_state.txt")

def varlong(b):
    n = s = 0
    while True:
        x = b.u8(); n |= (x & 0x7F) << s; s += 7
        if not x & 0x80: return n

def parse_tree(b):
    """ClientboundCommandsPacket -> list of (flags, children, name). Asserts the whole packet is consumed."""
    nodes = []
    for _ in range(b.varint()):
        flags = b.u8()
        kids = [b.varint() for _ in range(b.varint())]
        if flags & 8: b.varint()
        name = None
        if flags & 3:
            name = b.str()
        if flags & 3 == 2:
            p = b.varint()
            if p in (ARG["integer"], ARG["float_"], ARG["double_"], ARG["long_"]):
                f = b.u8()
                size = {ARG["integer"]: 4, ARG["float_"]: 4, ARG["double_"]: 8, ARG["long_"]: 8}[p]
                b.take(size * ((f & 1) + (f >> 1 & 1)))
            elif p == ARG["string_"]: b.varint()
            elif p in (ARG["entity"], ARG["score_holder"]): b.u8()
            elif p == ARG["time_"]: b.i32()
            elif p in (ARG["resource"], ARG["resource_key"], ARG["resource_or_tag"], ARG["resource_or_tag_key"], ARG["resource_selector"]): b.str()
            if flags & 16: b.str()
        nodes.append((flags, kids, name))
    root = b.varint()
    assert b.i == len(b.d), "trailing bytes in commands packet"
    return nodes, root

def join(name):
    """Login, capturing the commands tree and the first set_time (both are sent before the first teleport)."""
    import bot
    seen = {}
    orig = bot.play_step
    def step(c):
        pid, b = c.recv()
        if pid == PLAY_CB["commands"]: seen["tree"] = parse_tree(b)
        elif pid == PLAY_CB["set_time"] and "time" not in seen: seen["time"] = read_time(b)
        elif pid == PLAY_CB["player_position"]:
            tid, pos = b.varint(), b.take(24)
            b.take(24)
            c.pos = struct.unpack(">ddd", pos)
            c.send(PLAY_SB["accept_teleportation"], varint(tid) + pos + b.take(8))
        return pid
    bot.play_step = step
    try:
        c, _ = login(name, sys.argv[1], int(sys.argv[2]), view=2)
    finally:
        bot.play_step = orig
    return c, seen

def read_time(b):
    gt = b.i64()
    clocks = {}
    for _ in range(b.varint()):
        cid = b.varint(); total = varlong(b)
        partial, rate = struct.unpack(">ff", b.take(8))
        clocks[cid] = (total, rate)
    return gt, clocks

def cmd(c, line): c.send(PLAY_SB["chat_command"], string(line))
def chat_with(c, needle): return wait(c, PLAY_CB["system_chat"], lambda x: needle in x.d).d

def root_names(tree):
    nodes, root = tree
    return {nodes[k][2] for k in nodes[root][1]}

if __name__ == "__main__":
    after = "--after-restart" in sys.argv
    a, seen = join("Alice")
    names = root_names(seen["tree"])
    assert {"gamemode", "execute", "scoreboard", "op", "tp", "teleport"} <= names, names
    gt, clocks = seen["time"]
    assert 0 in clocks and 1 in clocks, clocks
    print(f"op tree ok: {len(seen['tree'][0])} nodes, {len(names)} commands; game time {gt}, day {clocks[0][0]}")

    if after:
        saved_gt, saved_day = map(int, open(STATE).read().split())
        assert gt >= saved_gt and clocks[0][0] >= saved_day, (gt, saved_gt, clocks[0][0], saved_day)
        cmd(a, "scoreboard objectives list")
        assert b"ctest" in chat_with(a, b"commands.scoreboard.objectives.list")
        print("time + scoreboard survived restart ok")
        sys.exit(0)

    # Time keeps running and survives a rejoin (one authoritative clock).
    time.sleep(1.2)
    a.s.shutdown(2)
    time.sleep(0.3)  # let the server notice the disconnect
    a, seen = join("Alice")
    gt2, clocks2 = seen["time"]
    assert gt2 >= gt + 15 and clocks2[0][0] >= clocks[0][0] + 15, (gt, gt2, clocks, clocks2)
    print(f"clock ok: {gt} -> {gt2}")

    # Non-op: filtered tree, op commands rejected with vanilla's error + context.
    c, cs = join("Carol")
    cnames = root_names(cs["tree"])
    assert "gamemode" not in cnames and "help" in cnames and "msg" in cnames, cnames
    cmd(c, "gamemode creative")
    d = chat_with(c, b"command.unknown.command")
    d2 = chat_with(c, b"command.context.here")
    assert b"gamemode creative" in d2, d2
    print(f"permissions ok: non-op sees {len(cnames)} commands")

    # Syntax error points at the bad argument.
    cmd(a, "time add banana")
    assert b"command.context.here" in chat_with(a, b"banana")

    cmd(a, "time set 1000")
    b = wait(a, PLAY_CB["set_time"], lambda x: len(x.d) > x.i + 9)
    _, cl = read_time(b)
    assert cl[0][0] == 1000, cl
    assert b"commands.time.set.absolute" in chat_with(a, b"commands.time.set")
    cmd(a, "time query time")
    assert b"commands.time.query.absolute" in chat_with(a, b"commands.time.query")
    print("time command ok")

    cmd(a, "setblock 1 -59 1 stone")
    wait(a, PLAY_CB["block_update"])
    cmd(a, "fill 0 -59 0 3 -59 3 oak_planks hollow")
    wait(a, PLAY_CB["section_blocks_update"])
    assert b"commands.fill.success" in chat_with(a, b"commands.fill")
    cmd(a, "fill 0 0 0 100 100 100 stone")
    assert b"commands.fill.toobig" in chat_with(a, b"commands.fill")
    print("setblock/fill ok")

    cmd(a, "scoreboard objectives add ctest dummy")
    chat_with(a, b"commands.scoreboard.objectives.add.success")
    cmd(a, "scoreboard players set Alice ctest 5")
    chat_with(a, b"commands.scoreboard.players.set.success")
    cmd(a, "execute as @a[name=Alice] if score @s ctest matches 5 run say pass")
    assert b"pass" in chat_with(a, b"chat.type.announcement")
    cmd(a, "execute if score Alice ctest matches 6")
    chat_with(a, b"commands.execute.conditional.fail")
    print("scoreboard + execute ok")

    cmd(a, 'bossbar add test:bar "Boss"')
    chat_with(a, b"commands.bossbar.create.success")
    cmd(a, "bossbar set test:bar players @a")
    wait(c, PLAY_CB["boss_event"])
    cmd(a, "bossbar set test:bar value 50")
    chat_with(a, b"commands.bossbar.set.value.success")
    print("bossbar ok")

    cmd(a, "msg Carol hi there")
    assert b"hi there" in chat_with(c, b"commands.message.display.incoming")
    cmd(a, "give Alice diamond 3")
    assert b"commands.give.success.single" in chat_with(a, b"commands.give")
    print("msg/give ok")

    # Suggestions for an ask_server node (op <targets>): online non-op players.
    a.send(PLAY_SB["command_suggestion"], varint(7) + string("/op C"))
    s = wait(a, PLAY_CB["command_suggestions"])
    assert s.varint() == 7
    start, length, n = s.varint(), s.varint(), s.varint()
    opts = []
    for _ in range(n):
        opts.append(s.str())
        assert not s.u8()  # no tooltip
    assert (start, length) == (4, 1) and opts == ["Carol"], (start, length, opts)
    print("suggestions ok")

    # op/deop resends the tree
    cmd(a, "op Carol")
    t = wait(c, PLAY_CB["commands"])
    assert "gamemode" in root_names(parse_tree(t))
    cmd(a, "deop Carol")
    t = wait(c, PLAY_CB["commands"])
    assert "gamemode" not in root_names(parse_tree(t))
    print("op/deop ok")

    cmd(a, "save-all")
    chat_with(a, b"commands.save.success")
    a.send(PLAY_SB["command_suggestion"], varint(8) + string("/time query "))
    open(STATE, "w").write("%d %d" % (gt2, 1000))
    print("commands ok")
