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

## In progress (branches)
| Branch | Scope |
|---|---|
| features | chunk-status pipeline, carvers, decoration features |
| structures | all structures, /locate |
| dimensions | Nether, End, portals |
| mobs-core | entity framework, first mobs, spawning, /summon |
| mobs-roster | remaining mobs, villagers, breeding |
| mobs-bosses | ender dragon, wither, warden |
| blockticks | scheduled/random ticks, fluids, falling blocks, door/bed physics |
| parity-audit | packet coverage, per-subsystem diffs vs vanilla, real-client log checks |
| perf-2 | tick / packet / entity tracking / worldgen hot paths, memory |
| bug-hunt | ASan/UBSan/TSan, fuzzing, bot stress, soaks |

## Left
- Redstone (wire, repeaters, comparators, pistons, observers, hoppers, droppers/dispensers, rails/minecarts)
- Block entities & containers (chests, furnaces, brewing, enchanting, anvils/grindstone/smithing, beacons, shulkers, signs/books, lecterns, jukeboxes, banners)
- Potions/effects, enchantments, XP, advancements, recipe book, statistics, maps
- Sleeping/beds/respawn anchors, weather, raids, sounds/particles parity, world border
- Anvil-format (vanilla region file) saving
- Online-mode auth + encryption + compression, signed chat, resource packs, server-list favicon, query, RCON
- Pi deployment guide (systemd), plugin API coverage
