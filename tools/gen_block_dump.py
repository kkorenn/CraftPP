#!/usr/bin/env python3
"""Build-time only: compile and run tools/blockdump/BlockDump.java against the vanilla server jar to write
data/block_states.bin (per-state rotate/mirror + flags; layout in BlockDump.java). The server never runs Java."""
import glob, os, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JDK = "/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home/bin"
cp = ":".join([f"{ROOT}/vanilla/versions/26.3/server-26.3.jar"] + glob.glob(f"{ROOT}/vanilla/libraries/**/*.jar", recursive=True))
with tempfile.TemporaryDirectory() as d:
    subprocess.check_call([f"{JDK}/javac", "-Xmaxerrs", "5", "-nowarn", "-cp", cp, "-d", d, f"{ROOT}/tools/blockdump/BlockDump.java"])
    r = subprocess.run([f"{JDK}/java", "-cp", cp + ":" + d, "BlockDump", f"{ROOT}/data/block_states.bin"], cwd=d,
                       capture_output=True, text=True)
    print((r.stdout + r.stderr)[-3000:] if r.returncode else r.stdout.strip().splitlines()[-1])
    sys.exit(r.returncode)
