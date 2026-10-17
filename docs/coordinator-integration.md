# Coordinator integration

Recovered work is integrated in an isolated `codex/coordinator` checkout, then
fast-forwarded into main after `./build.sh test` passes. Original worktrees and
uncommitted files are retained. Recovery copies were saved under
`/tmp/craftpp-coordinator-snapshot-20261017-090859`.

## Structures and foundation repairs

Main includes fortress/end-city pieces, structure edge updates, bounded caches,
and structure mob queues. Chunk transfer keeps fluid post-processing as well as
structure mobs. Three pre-existing defects were repaired during validation:

- Fluid-tag parsing used an array owned by a destroyed temporary JSON value.
- Flat-world lighting could release an initialized column's chunk handle, then
  dereference it when lighting resumed. Initialization now reacquires the handle.
- The mob extractor queried unbound fluid tags, encoding all fluids as undefined
  type 3. That overran entity current arrays and gave wrong path types. The
  build-time extractor now binds vanilla block/fluid tags before extracting data;
  the C++ loader rejects undefined fluid types.

Release and ASan/UBSan suites passed, including restart, fuzzing and stress.
Release plugin tests passed. CodeRabbit's initial structure integration review
returned no findings; later extractor/test repairs were checked locally.

## Startup performance

Integrated parallel spawn-terrain prefetch and stronghold-ring biome searches,
compiled-data reuse, lazy feature-pool loading, and collision-face/heightmap
predicates for spawn support. Player free-space checks still approximate non-air
blocks as full cubes; that existing limitation remains separate from support
height selection.

Three serial warm runs of `pipeline_test 12345 1 features` on the current macOS
arm64 host compared commit `908f017` with the startup integration. Nine full
chunks include their generation dependencies; this probe does not attach runtime
lighting or structures.

| Measurement | Previous median | Integrated median |
| --- | ---: | ---: |
| Generator initialization | 94 ms | 36 ms |
| Full chunk including dependencies | 32.1 ms | 31.3 ms |
| Peak resident memory | 46 MiB | 22 MiB |

All nine full-feature block fingerprints matched between versions. The 32-chunk
noise/surface fingerprint remained `e4e3969b1fe85d8b`. Biome parity hash, RNG/noise
references, material checks, and the full server suite passed. These are host
measurements, not Raspberry Pi measurements.

## Feature ordering and lighting writes

Recovered the feature branch's FIFO view tickets and x-major decoration layers,
and its comparison tool's isolated run directories. The comparison tool also
accepts `--craftpp` to select a binary. Terrain-prefetch exceptions now remove
failed tickets and wake waiters; the regression reproduces the previous timeout
and confirms a retry completes.

Generation writes and final palette compaction now hold the light engine mutex
once palettes are visible to lighting readers. This completes the unfinished
storage branch's lighting-lock change without its malformed draft callback.

Seed 12345, radius 1, two generation workers matched a real vanilla surface-only
run: 884736 blocks and 13824 biome cells, zero mismatches. A separate full-feature
pipeline probe measured 30.4 ms/chunk and 22 MiB peak RSS on macOS arm64, excluding
runtime lighting and structures. Fossil/template features and possible Java
HashSet tree-bin iteration differences remain known limitations.
