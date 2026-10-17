# CraftPP status

Updated by the manager session. Branches live in `../CraftPP-worktrees/<name>`.

## Done (on main)
- Login / configuration / play, chunk streaming, chat
- Full vanilla command system (94 commands)
- Survival: tick loop, items, loot, inventory/crafting, vitals
- Worldgen noise + surface stages, bit-exact vs vanilla
- Paper-style console, Bukkit/Paper plugins (embedded JVM, only when plugins/ has jars), persistence
- perf: paletted chunks (~21 KB), unloading, worldgen worker pool, compression, Pi bench (idle RSS 14.7 MiB, after join 33 MiB, join 1.7 s) — see docs/performance.md
- Lighting: vanilla sky + block light engine, 0 mismatches vs vanilla on surface/carver stages, persisted, ~0.25 ms/chunk. Per-state light table `src/gen/light_data.hpp` is generated at tool time by tools/gen_light_data.py (tools/LightDump.java reads the vanilla jar). The server never runs vanilla code.
- Worldgen pipeline (26.3 ChunkStatus, WorldGenRegion), carvers bit-exact, all overworld features except template/fossil (0.06–0.36% diff vs vanilla run-to-run noise 0.2–4.5%). `data/blocks.bin` generated at tool time by tools/gen_block_data.sh.
- perf-2: buffered reads, single-frame broadcast, get_ptr fast path (get_block 76→16 ns), density vectorized (worldgen −14–17%), idle wakeups 5→0.3/s, level.dat saved per 5 min.
- Structures: all overworld structures (random_spread, concentric rings, jigsaw + processors, legacy pieces), beardifier exact, /locate matches vanilla (29/29 types). Monument 0 block diffs; village 0.03% at full stage. `data/block_states.bin` generated at tool time by tools/gen_block_dump.py. Mutable piece state in world/structures/r.X.Z.bin (deviation from vanilla, documented).
- Parity audit: real 26.3 client stays connected (set_entity_data fix), keep-alive timeout, teleport confirm, vanilla movement checks, block acks, spawn protection, game modes, armor, 10 damage game rules, fall-damage modifiers, unsigned player chat, inventory click parity. Findings: side-session notes parity-audit.md.
- Bug hunt: ASan/UBSan/TSan builds (`./build.sh [test] asan|tsan`), test/stress.py (fuzz, cmdfuzz, 50+ bots, reconnect storms, soak), 15 crash/DoS/race fixes (NaN moves, unbounded inbox, 2 MiB per-connection prealloc, socket timeouts, deep SNBT, int-overflow UB → -fwrapv, ghost players).
- Mobs core (src/mobs/): entity manager, vanilla TrackedEntity tracking (players too), A* pathfinding, GoalSelector + Brain, NaturalSpawner, explosions, combat (cooldown/crits/sweep/PvP/armour/killer death messages), XP orbs, /summon + @e selectors; cow, pig, sheep, chicken, zombie, skeleton, spider, creeper. ~0.45 ms mspt at 100 mobs. Tool-time extractor: tools/gen_mob_data.py + MobDump.java → data/mob_dump.json.
- Block ticks (src/level/): Level.setBlock + CollectingNeighborUpdater (vanilla update order), scheduled ticks (persisted), exact water/lava flow + worldgen fluid post-processing (vanilla-vs-CraftPP fluid diffs at vanilla-vs-vanilla level), random ticks (crops, grass, leaves, ice, snow, fire, copper), pop-offs, two-part blocks, buckets, bone meal, hoe, flint and steel, falling blocks. Tool-time extractor: tools/gen_level_data.py → data/level.json.
- Placement (src/level/place_rules.hpp): vanilla getStateForPlacement + updateShape/canSurvive for walls, chests, wire shape, rails (RailState), rotation-16 signs/banners/skulls, wall/hanging variants, attached blocks, count increments, per-class facing, waterlogging. Fixed state-id ordering for 12 blocks in data/game.json and item→block mapping for redstone/string/seeds.

- Coordinator integration: structure edge-shape updates, fortress/end-city pieces, bounded structure caches and structure mob queues. Fluid post-processing is preserved alongside mob queues when transferring generated chunks.
- Memory/data repairs: keep fluid-tag JSON alive during iteration; reacquire released initialized light-column chunk handles; bind vanilla block/fluid tags in the build-time mob extractor before querying fluid/path data. Generated mob data now distinguishes water/lava correctly; invalid type 3 is rejected at load time.
- Regression fixtures: cauldron checks synchronize requests and filter bucket sounds; water timing is measured in server ticks; cow combat selects the intended summoned entity by position.

- Startup performance: parallel spawn terrain and stronghold-ring searches, compiled-data reuse and lazy feature-pool loading. Warm generator init 94→36 ms and peak RSS 46→22 MiB on macOS arm64, unchanged terrain/feature fingerprints; see docs/coordinator-integration.md. Done: new world 1.5–1.9 s → 0.19–0.24 s (M4), 4.4 → 1.05 s (Pi-like Docker, 1.5 CPU); restart 0.08–0.10 s. Spawn y uses vanilla getLevelRespawnPos predicates. Startup warnings go through the logger. Stronghold rings are computed lazily (about 1 s pause on a Pi at the first far chunk or /locate stronghold).

- Client selectors: resend permission status and command tree after death/respawn, in vanilla packet order; regression covers `@s` and `@a`.

- Feature ordering: FIFO generation tickets and x-major decoration layers, failed-prefetch recovery, synchronized palette writes visible to lighting. Surface-only comparison seed 12345/radius 1 matched all blocks and biomes.

- Dimensions: recovered Nether/End pipelines and portal travel, per-world scheduled/random ticks and structure managers; repaired Nether-fossil column bounds, world-specific lava rules and cross-world caches. Sampled Nether/End surface and Nether structure stages match vanilla; see docs/coordinator-integration.md for scope and remaining gaps.
- Storage (from codex/coordinator-storage): vanilla Anvil world format — region files, chunk NBT, level.dat + SavedData, players/data NBT, entities in Anvil, proto-chunk eviction + persistence, light column unloading.

## In progress (branches)
| Branch | Scope |
|---|---|
| roster-int | integrating mobs-roster (78 mobs, villagers, taming, horses) onto main + slime-cap test fix |
| mobs-bosses | ender dragon, wither, warden |
| effects | mob effects, potions + brewing results, enchantment effects (all), XP/levels parity |
| redstone | wire, torches, repeaters, comparators, observers, pistons, hoppers, droppers/dispensers, rails |
| containers | block entities + menus; porting ender chest/item components to NBT player data, then merge |
| feature-parity | Further full-stage comparisons across biomes; fossil/template features and tree-bin iteration |
| dim-light | per-World light engine (nether no skylight, end), light_ensure stall, TSan re-check |
| nether-end-gen | nether/end feature types, end gateways, nether/end natural spawning |
| dim-bughunt | end-portal segfault (ASan), portal destinations vs vanilla, pick-block reach, duplicate UUID kick, flaky tests |

## Left
- Saplings → trees, huge mushrooms, grass bone meal (features hooks); TNT, lightning, cauldrons; 26.3 timeline sky darkening
- Advancements, recipe book, statistics, maps
- Sleeping/beds/respawn anchors, weather, raids, sounds/particles parity, world border
- Online-mode auth + encryption + compression, signed chat, resource packs, server-list favicon, query, RCON
- Pi deployment guide (systemd), plugin API coverage

## Known issues
- Nether/End lighting still uses full-light placeholders; several Nether generation features and dimension environment attributes remain incomplete.
- Movement speed checks are covered by parity_test.py; pick_item_from_block reach handling still needs audit.
- Duplicate UUID with two names can be online at once.
- Flaky: plugin_test second start under heavy load; test-server.log occasionally misses lines after restart.
- vanilla/client (real client for tools/run_client.py) was lost on 2026-10-17 and is not restored; vanilla/ server data was rebuilt from the verified server.jar.
- Light: tick thread waits on the light mutex in light::ensure_lit (113 ms spike with 400 fluid sources).
