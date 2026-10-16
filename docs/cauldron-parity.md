# Cauldron bucket parity

Bucket interactions ported from 26.3 `CauldronInteractions.java` and
`ItemUtils.createFilledResult` into `src/level/cauldrons.hpp`.

- Water, lava, and powder-snow buckets replace any cauldron contents.
- Water and powder snow fill to level 3; lava has no level property.
- Empty buckets extract lava, or water/powder snow only at level 3.
- Water above the cauldron (including waterlogged blocks) prevents lava and
  powder-snow filling. This consumes the interaction without swinging or
  changing either the cauldron or inventory.
- Both hands work; sneaking with held items bypasses block interaction.
- Survival consumes one bucket and returns the corresponding result, using
  inventory insertion and dropped leftovers for stacked buckets.
- Creative preserves the held stack and adds the result only when absent.
- Successful fills and extractions broadcast the corresponding bucket sound.

`test/cauldron_test.py`, included in `./build.sh test`, checks block states,
held stacks, stacked remainders, creative deduplication, offhand interactions,
waterlogged underwater checks, swing suppression, sneaking bypass, and sound
packets against these source rules.
No new generation stage or per-tick scan is added.

Remaining cauldron work: potion/bottle interactions, dye/banner/shulker washing,
precipitation, dripstone filling, entity effects, statistics and game events.
Potion and washing interactions require item components beyond the current
component-free bucket representation.
