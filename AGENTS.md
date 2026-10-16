# CraftPP agent rules

CraftPP is a Minecraft Java Edition **26.3** (protocol 777) server written in C++20.

## Goals (in priority order)
1. **Behaviour identical to vanilla.** World generation must be bit-exact for a given seed.
2. **Runs well on a Raspberry Pi.** Memory, CPU (idle and loaded) and latency all matter. Portable to Linux aarch64.
3. **Full C++ port.** Never call vanilla code at runtime (no JVM helpers). The only exception is the optional Bukkit plugin JVM.

## Sources of truth
- `vanilla/src/net/minecraft/`: decompiled, unobfuscated 26.3 source. 26.x rewrote a lot (worldgen, material rules, density functions), so port from this, not from knowledge of older versions.
- `vanilla/src/data/minecraft/`: vanilla datapack JSON (worldgen, loot, recipes, tags).
- `vanilla/generated/reports/`: blocks.json, registries.json, packets.json, commands.json, biome_parameters.
- Bytecode check: `javap -c -p -classpath vanilla/versions/26.3/server-26.3.jar <class>`. The decompiler prints some double constants with an `F` suffix; check the bytecode whenever float/double typing matters.
- The volume path (`sampleVolume`) and the point path (`sampleValue`) can give different floats. Use whichever path vanilla uses at each call site.

## Workflow
- Work only in your own git worktree under `/Users/koren/Documents/CraftPP-worktrees/<name>`. Never edit `/Users/koren/Documents/CraftPP` (main); the coordinator merges.
- Commit small working steps: `git -c user.name=CraftPP -c user.email=craftpp@localhost commit`. Every commit must pass `./build.sh test`, which picks a free port per run.
- `git merge main` periodically to pick up other agents' merged work. Resolve conflicts carefully.
- Keep edits to shared files (src/server.hpp, play.hpp, main.cpp, world.hpp, CMakeLists.txt) small and targeted. Put new subsystems in their own files or directories.
- Never kill processes by broad patterns (`pkill craftpp`): other agents' servers run in parallel. Kill by PID or by port.

## Architecture
- **Threads:** one tick thread owns all game state at 20 TPS (src/game/tick.hpp has the header docs). Network threads only read packets into per-player queues. `Player::send()` appends to an outbox that the tick thread flushes. From other threads, use `run_on_tick(fn)`.
- **Hooks:** `on_tick`, `on_player_tick`, `on_player_join`, `on_player_leave`, and the packet handler registry `on(play_sb::X, fn)` (src/play.hpp).
- **Generated data:** `src/gen/ids.hpp` comes from `tools/gen_data.py`, `src/gen/command_data.hpp` from `tools/gen_commands.py`, and `data/game.json` from `tools/gen_game_data.py`. Extend the generators; don't hand-edit their output.
- **Worldgen:** lives in `src/worldgen/`.
  - `density.*`: density functions.
  - `terrain.*`: TerrainGenerator, NoiseChunk, aquifers, biomes, spawn finder.
  - `material.*`: 26.3 material rules (surface).
  - `biome_source.*`: multi-noise biome lookup.
  - `noise.*`, `random.*`, `json.hpp`, `mth.hpp`: noise, RNG, JSON, math.
- **Chunks:** `src/world.hpp` (Chunk, World). The perf branch is changing the in-memory storage to paletted, adding unloading and moving generation to a worker pool. Access blocks through `Chunk::get/set/set_biome` and `World::get_block/set_block`, never through raw arrays.

## Verifying against vanilla
- `python3 test/compare.py --seed S --radius R [--vanilla-datapack DIR] [--mode full|solid]` runs a real vanilla server as a reference (only to compare output) and diffs blocks and biomes chunk by chunk.
- `tools/make_noise_only_datapack.py --stage noise|surface|full` builds datapacks that disable later generation stages in vanilla.
- **Expected noise:** vanilla's feature stage is not deterministic across runs. Generation order changes about 0.7% of blocks. Use stage datapacks for exact checks, and judge feature-stage results against vanilla-vs-vanilla variance.
- **Current state:** the noise and surface stages match vanilla. The remaining differences are fluids that vanilla lets flow after generation.

## Performance
- **Measure:** report ms/chunk and resident memory for new stages.
- **Memory:** no per-block heap allocation. Free parse-time JSON once compiled.
- **CPU:** idle CPU about 0%, no busy-wait threads.
