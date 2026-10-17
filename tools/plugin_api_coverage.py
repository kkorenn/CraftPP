#!/usr/bin/env python3
"""Audit native JSON bridge dispatch coverage, not total Bukkit/Paper compatibility.

Denominator: unique literal Main.rpc.api("route") calls in bridge Java sources.
Numerator: routes with C++ dispatch branches, excluding known placeholder branches.
Partial routes remain in the wired numerator, but are listed separately. Java-only API
methods, overloads, events, fast JNI calls and NMS are outside this metric. Having a
handler does not certify correct behavior: use plugin_test.py for runtime validation.
"""
import argparse
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
    "server.registries": "Null; Java falls back to bundled registry report",
}
PARTIAL = {
    "world.setGameRule": "Shared rule state across dimensions; command changes do not emit Bukkit events",
    "entity.setPluginData": "Session only; entity IDs not included in storage key",
    "player.setPluginData": "Session only",
    "player.giveExp": "Changes total XP without recomputing level/progress",
    "player.teleport": "Dimension argument ignored",
    "world.setPluginData": "Session only",
    "world.setChunkPluginData": "Session only",
    "world.chunkPluginData": "Session only",
    "world.pluginData": "Session only",
    "world.highestY": "Non-air scan; heightmap overload selection unsupported",
    "world.setTime": "TimeSkipEvent not emitted",
    "world.setSpawn": "SpawnChangeEvent not emitted",
    "world.setBorder": "Border change events not emitted; single shared server border",
    "world.setBlock": "applyPhysics overload argument ignored by bridge",
    "world.setBlockData": "applyPhysics overload argument ignored by bridge",
}


def audit(revision=None):
    def read(path):
        if revision:
            return subprocess.check_output(["git", "show", f"{revision}:{path.relative_to(ROOT)}"], cwd=ROOT, text=True)
        return path.read_text()
    routes = set()
    for path in (ROOT / "java/src/main/java/prism/bridge").rglob("*.java"):
        text = read(path)
        routes.update(re.findall(r'Main\.rpc\.api\(\s*"([\w.]+)"', text))
        # helpers that build {m, a} payloads: send("scoreboard.x", ...), call("bossbar.x", ...), payload "m"
        routes.update(re.findall(r'\b(?:send|call)\(\s*"((?:scoreboard|bossbar)\.[\w.]+)"', text))
        routes.update(re.findall(r'addProperty\("m",\s*"([\w.]+)"\)', text))
    source = read(ROOT / "src/plugins/plugins.cpp")
    source = source.split("std::string api(", 1)[1].split("// ---------- natives", 1)[0]
    for extra in sorted((ROOT / "src/plugins").glob("api_*.cpp")):  # route groups api() falls through to
        source += "\n" + read(extra)
    # Ignore C++ comments, so a commented-out branch never counts as implemented.
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    handlers = set(re.findall(r'\bm\s*==\s*"([\w.]+)"', source))
    # prefix-dispatched groups: op/method switch inside one handler (scoreboard.*, bossbar.*)
    handlers |= {r for r in routes if r.split(".")[0] in ("scoreboard", "bossbar") and f'"{r}"' in source}
    stale = (STUBS.keys() | PARTIAL.keys()) - routes
    if stale:
        raise SystemExit("Coverage classifications need updating: " + ", ".join(sorted(stale)))
    missing = routes - handlers
    placeholders = set(re.findall(r'if\s*\(\s*m\s*==\s*"([\w.]+)"\s*\)\s*return\s*"(?:[^"\\]|\\.)*"\s*;', source))
    stubs = routes & (STUBS.keys() | placeholders) & handlers
    wired = routes & handlers - stubs
    partial = wired & PARTIAL.keys()
    return {
        "metric": "Native JSON bridge routes with non-placeholder dispatch handlers",
        "scope": "Literal Main.rpc.api calls only; excludes Java-only APIs, overloads, events, fast JNI, NMS",
        "total": len(routes), "wired": len(wired), "wired_percent": round(100 * len(wired) / len(routes), 1),
        "wired_without_listed_gaps": len(wired - partial),
        "partial": {key: PARTIAL[key] for key in sorted(partial)},
        "stubs": {key: STUBS.get(key, "Constant placeholder response") for key in sorted(stubs)},
        "missing": sorted(missing),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--revision", help="Audit a Git revision instead of current source")
    args = parser.parse_args()
    result = audit(args.revision)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(f"Native bridge wiring: {result['wired']}/{result['total']} ({result['wired_percent']}%)")
        print(f"Wired routes: {result['wired_without_listed_gaps']} without listed gaps, {len(result['partial'])} partial")
        print(f"Stub routes: {len(result['stubs'])}; missing routes: {len(result['missing'])}")
        print("Not a percentage of all Bukkit/Paper APIs. " + result["scope"])
        for group in ("partial", "stubs", "missing"):
            print(group + ":")
            entries = result[group]
            for route in entries:
                print("  " + route + (": " + entries[route] if isinstance(entries, dict) else ""))
