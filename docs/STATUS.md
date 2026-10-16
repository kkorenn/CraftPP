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

## In progress (branches)
| Branch | Scope |
|---|---|
| structures-2 | structure edge shape updates, nether fortress/bastion/end city pieces, bounded structure caches, structure entities hooks |
| dimensions | Nether, End, portals |
| mobs-core | entity framework, first mobs, spawning, /summon |
| mobs-roster | remaining mobs, villagers, breeding |
| mobs-bosses | ender dragon, wither, warden |
| blockticks | scheduled/random ticks, fluids, falling blocks, door/bed physics |
| parity-audit | packet coverage, per-subsystem diffs vs vanilla, real-client log checks |
| bug-hunt | ASan/UBSan/TSan, fuzzing, bot stress, soaks |
| containers | block entities + container menus |
| storage | vanilla Anvil region files, proto-chunk eviction |
| startup-perf | parallel spawn-region generation, fast restart (worldgen data cache), startup log hygiene |

## Left
- Redstone (wire, repeaters, comparators, pistons, observers, hoppers, droppers/dispensers, rails/minecarts)
- Potions/effects, enchantments, XP, advancements, recipe book, statistics, maps
- Sleeping/beds/respawn anchors, weather, raids, sounds/particles parity, world border
- Online-mode auth + encryption + compression, signed chat, resource packs, server-list favicon, query, RCON
- Pi deployment guide (systemd), plugin API coverage
