#!/usr/bin/env python3
"""Per-state light properties -> src/gen/light_data.hpp, dumped from the vanilla jar by tools/LightDump.java.
Light emission, dampening and face occlusion shapes come out of lambdas and voxel-shape code (stairs, slabs,
snow layers ...) that no report contains, so this reads them from the real BlockState cache instead of
re-implementing VoxelShape. Tool-time only; needs a JDK 25."""
import glob, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JAVA = os.environ.get("JAVA", "/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home/bin/java")
cp = [f"{ROOT}/vanilla/versions/26.3/server-26.3.jar"] + glob.glob(f"{ROOT}/vanilla/libraries/**/*.jar", recursive=True)
out = f"{ROOT}/src/gen/light_data.hpp"
subprocess.run([JAVA, "-cp", os.pathsep.join(cp), f"{ROOT}/tools/LightDump.java", out], check=True, cwd=tempfile.mkdtemp())  # vanilla writes logs/ into cwd
print("wrote", out)
