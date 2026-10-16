# CraftPP status

Updated by the manager session. Branches live in `../CraftPP-worktrees/<name>`.

## Done (on main)
- Login / configuration / play, chunk streaming, chat
- Full vanilla command system (94 commands)
- Survival: tick loop, items, loot, inventory/crafting, vitals
- Worldgen noise + surface stages, bit-exact vs vanilla
- Paper-style console, Bukkit/Paper plugins (embedded JVM, only when plugins/ has jars), persistence

## In progress (branches)
| Branch | Scope |
|---|---|
| perf | Pi benchmarks (aarch64 Docker), paletted chunks, unloading, worldgen worker pool, network |
| features | chunk-status pipeline, carvers, decoration features |
| structures | all structures, /locate |
| lighting | vanilla light engine |
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
