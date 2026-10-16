#!/usr/bin/env python3
"""Downloads the vanilla client from Mojang (official) and joins localhost in offline mode.
Usage: python3 tools/run_client.py [version] [server]   -> logs in vanilla/client/run/logs/latest.log"""
import json, os, platform, subprocess, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VER = sys.argv[1] if len(sys.argv) > 1 else "26.3"
SERVER = sys.argv[2] if len(sys.argv) > 2 else "localhost:25565"
DIR = os.path.join(ROOT, "vanilla", "client")
JAVA = "/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home/bin/java"
OS = {"Darwin": "osx", "Linux": "linux", "Windows": "windows"}[platform.system()]


def fetch(url, path):
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        urllib.request.urlretrieve(url, path + ".part")
        os.rename(path + ".part", path)
    return path


def allowed(rules):
    ok = not rules
    for r in rules or []:
        if "os" in r and r["os"].get("name") != OS:
            continue
        if "features" in r:
            continue
        ok = r["action"] == "allow"
    return ok


manifest = json.load(urllib.request.urlopen("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"))
meta = json.load(open(fetch(next(v["url"] for v in manifest["versions"] if v["id"] == VER),
                            os.path.join(DIR, VER + ".json"))))
cp = []
for lib in meta["libraries"]:
    if not allowed(lib.get("rules")):
        continue
    art = lib["downloads"].get("artifact")
    if art:
        cp.append(fetch(art["url"], os.path.join(DIR, "libraries", art["path"])))
cp.append(fetch(meta["downloads"]["client"]["url"], os.path.join(DIR, "client-" + VER + ".jar")))

# Asset index + objects (sounds, languages). Needed for a clean start.
idx = meta["assetIndex"]
idx_path = fetch(idx["url"], os.path.join(DIR, "assets", "indexes", idx["id"] + ".json"))
for obj in json.load(open(idx_path))["objects"].values():
    h = obj["hash"]
    fetch(f"https://resources.download.minecraft.net/{h[:2]}/{h}", os.path.join(DIR, "assets", "objects", h[:2], h))

run = os.path.join(DIR, "run")
os.makedirs(run, exist_ok=True)
args = [JAVA, "-Xmx2G"]
if OS == "osx":
    args.append("-XstartOnFirstThread")
args += ["-cp", os.pathsep.join(cp), meta["mainClass"],
         "--username", "Tester", "--version", VER, "--gameDir", run,
         "--assetsDir", os.path.join(DIR, "assets"), "--assetIndex", idx["id"],
         "--accessToken", "0", "--userType", "legacy", "--quickPlayMultiplayer", SERVER]
print("launching", VER, "->", SERVER)
subprocess.run(args, cwd=run)
