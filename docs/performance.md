# Performance (Raspberry Pi target)

Target: Raspberry Pi 4/5 (ARM64 Linux, 1-4 GB, Cortex-A72/A76). Memory, CPU (idle and loaded) and
responsiveness matter equally.

## Benchmark

`tools/bench.sh` builds a Linux aarch64 Release binary in Docker (`tools/docker/Dockerfile` = Debian bookworm,
the Raspberry Pi OS toolchain: GCC 12, Clang 14, CMake 3.25), starts it in a container limited like a Pi 4
(`--cpus=1.5 --memory=1g`, swap off) on a fresh normal world (seed 12345, view distance 8) and drives it with a
bot (`test/bot.py`) from the host. `tools/bench.py` does the measuring:

```
tools/bench.sh                         # GCC build, default limits
tools/bench.sh --cc clang              # Clang build
CMAKE_ARGS=-DCRAFTPP_CPU=cortex-a72 tools/bench.sh --cpus 0.5 --walk 20 --speeds 10,40
```

| metric | how |
|---|---|
| idle RSS | `/proc/1/status` VmRSS 6 s after start (after the first autosave) |
| join | bot login until `player_position`; the server sends all 289 chunks before it |
| join bytes | container network tx during the join |
| CPU % | utime+stime from `/proc/1/stat` over 10 s (100 % = one core) |
| walk | bot moves east at 10 b/s (creative flight) then 40 b/s (elytra-ish) for 30 s each into new terrain |
| /list round trip | one `/list` per second while walking: tick-thread responsiveness (tick = 50 ms, so ~25 ms is the floor) |
| chunks missing / catch-up | view-square chunks the bot lacks when it stops, and time until it has them all |
| generator ms/chunk | `terrain_test` (single thread, 32 chunks) in the same limits |

Caveats: the host is an M4 Mac. Docker `--cpus` limits *how much* CPU, not how fast one core is: an A72 core
is roughly 3-5x slower than an M4 core, so on a real Pi 4 expect generation (and join time) to be ~3-5x these
numbers; memory numbers transfer directly. The host is shared with other work, so timings vary by ~15 %.
`--cpus 0.5` is a rough stand-in for a Pi's total throughput.

## Results

Baseline = main at a246f47 (tick-thread architecture, before this branch); after = branch `perf`.
`--cpus=1.5 --memory=1g`, GCC 12 build.

| metric | baseline | after |
|---|---|---|
| idle RSS, 0 players | 64.6 MiB | **14.7 MiB** |
| CPU idle, 0 players / 1 idle player | 0.0 % / 0.1 % | 0.2 % / 0.1 % |
| join, 289 chunks | 3.61 s | **1.73 s** (Clang: 1.62 s) |
| join: bytes sent | 20.1 MiB | **1.2 MiB** |
| RSS after join (1 player) | 123.2 MiB | **33.4 MiB** |
| walk 10 b/s 30 s: CPU / RSS at end | 13 % / 194 MiB | 15 % / **37 MiB** |
| walk 10 b/s: /list round trip avg / max | 84 / 242 ms | **28 / 51 ms** |
| walk 40 b/s 30 s: CPU / RSS at end | 41 % / 455 MiB | 43 % / **39 MiB** |
| walk 40 b/s: /list round trip avg / max | 71 / 194 ms | **28 / 47 ms** |
| chunks missing when stopping | 0 (tick thread blocked instead) | 0 |
| peak RSS (VmHWM) | 455 MiB | **39 MiB** |
| generator noise / noise+surface, ms/chunk | 3.2 / 9.2 | 3.6 / 9.2 (Clang: 3.5 / 7.5) |

Stress, `--cpus=0.5` (walk 20 s):

| metric | baseline | after |
|---|---|---|
| join | 5.60 s | 4.96 s |
| walk 40 b/s: /list avg / max | 200 / 490 ms | 40 / 80 ms |
| walk 40 b/s: RSS at end | 346 MiB | 34 MiB |

RSS no longer grows with explored area: chunks outside every view are unloaded (after saving) at each 5 s
autosave. Generation CPU per chunk is about the same; the gain is that it no longer runs on the tick thread
(baseline: a 289-chunk respawn or a walk into new terrain stalled the tick for up to ~0.5 s on 0.5 CPU).

## What changed

**Chunk storage (src/world.hpp).** Sections are vanilla-style paletted containers: single value (all-air
sections cost nothing), 4-8 bit palette, or 16-bit global ids; biomes 1-3 bit palette or global. Entries never
span longs, so the in-memory longs are the network and disk encoding. Generated chunks are compacted (stale
palette entries dropped). ~21 KB per generated chunk instead of 200 KB. `Chunk::get/set/set_biome` keep
their semantics; `get_biome` added.

**Chunk lifecycle.** `World` holds `shared_ptr<Chunk>` plus a map of in-flight `shared_future`s: a chunk is
loaded/generated exactly once, by whoever asks first (`request()` = on the worker pool, `get_ptr()` = inline);
everyone else waits on the same future. `ViewTicket` (a Player member) counts each player's view square;
`save_dirty()` saves dirty chunks and unloads chunks no view contains and nothing else holds (saving them
first if they were never stored). `malloc_trim` afterwards hands the memory back. Queued generation of chunks
that left every view before a worker got to them is cancelled.

**Streaming (src/server.hpp).** `update_view` sends forgets immediately, requests new chunks nearest first
(squared distance, like vanilla) and queues the futures on the player. `deliver_chunks` (an `on_player_sync`
hook, tick thread) sends finished chunks nearest first, at most 32 per tick, inside chunk batches. The tick
thread never generates or encodes a new chunk. During join (network thread, outbox not live yet) it waits for
all chunks so the client has them before `player_position` (tests rely on this; vanilla sends them after).

**Worker pool.** One thread per CPU of the cgroup quota (`/sys/fs/cgroup/cpu.max`) or core count, max 4, at
nice +5 so the tick thread preempts them. `worker-threads=N` in server.properties overrides. Workers load or
generate, compact, and encode + compress the chunk's packet.

**Disk (chunk files).** `world/chunks/c.X.Z.bin` = `CPP2` + zlib (level 1) of the sections in network form;
`CPP1` (old raw format) is still read. A generated chunk is 1.9 KB on disk. Not Anvil: vanilla region files
need NBT with block-state *names* per palette, heightmaps, status, etc.: a bigger step, left for when vanilla
world import/export matters (the section encoding here is already the same bits, only the container differs).

**Caching generated chunks on disk: yes.** Measured per chunk (M4): load 0.065 ms, save 0.3 ms, generate 9 ms
(Linux container; ~30-45 ms on a Pi 4). Loading is >100x cheaper than regenerating for 1.9 KB of SD card, and
a walk back over explored ground costs almost nothing. Chunks are written once, when first unloaded (not when
generated), so a short visit costs no write. Vanilla keeps everything on disk too; features, structures and
lighting will only make regeneration costlier. `World::cache_generated` switches it off.

**Network (src/net.hpp, Player::send).** Writes were already batched by the survival branch (one non-blocking
send per player per tick). Added: vanilla compression (`network-compression-threshold`, default 256 like
vanilla, -1 disables) with zlib level 1. Chunk packets are cached per chunk as the final frame (compressed
~2.7 KB, from 75 KB: 53 KB of every packet is full sky light, which compresses to almost nothing) until a block
changes; the uncompressed path writes the frame length in place, no extra copy of the payload.

| zlib level | ms per chunk packet (M4, 1 core) | bytes (75 KB raw) |
|---|---|---|
| 1 | 0.10 | 2758 |
| 4 | 0.20 | 2199 |
| 6 (vanilla default) | 0.34 | 2094 |

Compression costs ~1 % of the chunk's generation time, once per chunk, on a worker; it cuts join traffic
20 MiB -> 1.2 MiB (a 289-chunk join is ~6 s of a 25 Mbit/s Pi Wi-Fi link uncompressed) and the join's outbox
burst by the same factor (RSS after join 61 -> 33 MiB). **Recommendation: keep it on (256).** Disable only
for a LAN-only server where every millisecond of CPU matters.

**Worldgen (exact).** MaterialSystem no longer parses the 7 MB blocks report into a DOM: it indexes the
top-level keys and parses only the ~40 blocks the rules name (that was the ~49 MB; idle RSS 64.6 -> 14.7 MiB). Surface stage: the biome zoom's per-corner offsets (BiomeManager fiddle, 8 LCG chains per
block) are memoized per chunk; neighbours' biome grids are reused from the chunks that computed them (a
bounded cache of 512 grids, ~0.8 MB) instead of being recomputed for every chunk, which is also what vanilla's
WorldGenRegion does. Profiled with `perf` in the container (`--privileged`): remaining hot spots are the density
function interpreter (BinS/RangeChoice/Interpolated), Perlin `addToVolume`, Aquifer and the zoom's distance loop.

**Build.** Release is `-O2` (O3 measured no faster, larger binary), `-ffp-contract=off` kept for every target.
`-DCRAFTPP_CPU=cortex-a72` (Pi 4) / `cortex-a76` (Pi 5) adds `-mcpu`; no `-march=native` by default.
`-DCRAFTPP_LTO=ON` exists but is off: -0.1 MB binary, no measurable speed-up with GCC, and Clang 14 LTO needs
the LLVM gold plugin / lld that bookworm doesn't install by default. Clang 14 needed a fix in `Json` (special
members defined out of line). **Clang is ~20 % faster than GCC 12 on the surface stage** (7.5 vs 9.2 ms/chunk);
`CXX=clang++ ./build.sh` on the Pi is worth it.

| build (container, 1.5 CPU) | noise | noise+surface | fingerprint |
|---|---|---|---|
| GCC 12 -O2 | 3.6 | 10.9 | e4e3969b1fe85d8b |
| GCC 12 -O3 | 3.6 | 11.4 | e4e3969b1fe85d8b |
| GCC 12 -O2 LTO | 3.6 | 10.8 | e4e3969b1fe85d8b |
| GCC 12 -O2 -mcpu=cortex-a72 | 3.6 | 11.0 | e4e3969b1fe85d8b |
| Clang 14 -O2 | 3.3 | 8.7 | e4e3969b1fe85d8b |

(Run before the zoom/neighbour-biome work; same session, so comparable with each other.)

## Linux

`./build.sh test` passes inside the bookworm container with GCC 12 (whole suite: bot, persist, survival,
commands; plugins are compiled out without a JDK). Clang 14 builds it too. SIGINT/SIGTERM (`docker stop`) save
and exit cleanly. Remaining Linux warnings are all `-Wunused-function` in plugins.cpp.

## Exactness

- `terrain_test` prints a fingerprint (FNV over every block and biome of 32 generated chunks). macOS (Apple
  clang), Linux GCC 12 and Clang 14 at every flag combination above print the same `e4e3969b1fe85d8b`, so the
  Linux builds generate the same bits as the macOS build that compare.py validates.
- `test/compare.py` with `tools/make_noise_only_datapack.py` packs (noise_only, surface_only), seeds 12345 (radius
  3 and 4) and 777 (radius 3), after every worldgen-touching commit: 0 biome mismatches; the only block mismatches
  are vanilla-side flowing water (`water[level=1..8]` vs air: vanilla ticks fluids), 0-19 per run, same kind and
  count range as the baseline binary (17 at seed 12345 radius 4).

## Recommendations for other subsystems

- `World::get()` still returns `Chunk&` for compatibility; a chunk can now be unloaded by the autosave thread
  once no player views it. Use `world.get_block/set_block` or hold a `ChunkPtr` from `world.get_ptr()`.
- Entities/physics touching unloaded chunks (`get_block` on a chunk nobody views) generate it synchronously on
  the tick thread; entity code should skip chunks that aren't loaded (vanilla does: no ticking outside loaded
  chunks). A `World::get_if_loaded` would be the natural next API.
- Lighting: every chunk packet carries 53 KB of full sky light. A real light engine should store per-section
  light only where it isn't uniform (vanilla sends empty/full masks).
- Packet ids/frames: `Player::send` frames (and maybe compresses) per call; pre-framed blobs passed to
  `send_raw_bytes` are re-framed when compression is on, so build them once at startup (like commands_packet).
- plugins.cpp: on Linux without a JDK six functions are unused (`-Wunused-function` warnings); wrap them in the
  same `#if CRAFTPP_JNI`.
