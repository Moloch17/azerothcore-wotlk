# 0027: Search that transfers: two maps trained, one held out, the search terms from the seat's own map; and the movement pacing amendments

**Date:** 2026-10-10 (owner: "Go ahead and build this plan"; design in `.agents/plans/general-search/CONTRACT.md`, the
lead's answers at its end, and the movement-pacing items M1-M8 folded into the same rebuild from
`.agents/plans/movement-pacing/movement-pacing.ANALYSIS.md`)

**Decision.** The seek stage (M2) trains on the Stockades (34) and Ragefire Chasm (389) 1:1 and is measured on the
Deadmines (36), held out, and the terms that teach searching come from the seat's own map, not the room table, so the
same lesson holds in a dungeon the table never described. The learner-side half (the coverage encoder, the held-out
headline `found_heldout_map`, the `no_coverage` arm, `look_entropy_coef` 0.01) is the Python stream's and recorded in
the learner docs; this file is the sim half, with the movement-pacing amendments.

1. **Per-map tables stay, as placement geometry.** `ArenaProblem` needs rooms and spawn points, `Place`, `TrapPose`
   and the rungs read them; the table is never observed (0019), so it costs nothing in generalisation. Ragefire (28
   rooms, 4 front) and the Deadmines (33 rooms) were authored offline by the Stockades' method extended to caves and
   stacked floors (`SeekTables.cpp` says how and gives the checks) and checked in as C++ data. An env's episode draws
   its arena and so its map; a Seek env that moved to another map clears the new instance when it opens (it did not:
   a level-1 seat in live Ragefire), a seek arena off the Stockades to the dungeon radius. `seek_room` indexes the
   union of the distinct tables; `arena_<name>` / `found_arena_<name>` and `seek_map` say which; the manifest's arenas
   carry `map_id`, `rooms`, `table_terms`. A seeded, unpinned evaluation episode goes round the trainable arenas.
2. **The map-derived terms** read the coverage analysis of the crop the seat already has (`Coverage::Analyse`:
   frontier clusters; chambers, the open floor eroded by a cell; pockets, chambers behind a narrowing reached over open
   cells). `FrontierClear` 0.1 pays a cluster's 10-yd bin cleared by positive evidence (the bin wholly known and
   frontier-free within 36 yd, no cluster left near it); `PocketEntry` 0.3 pays a pocket entered that the seat had
   seen as a separate chamber; both Exploring, each in an existing cap (`FrontierCap`, `RoomEntryCap`): the paid
   ceiling stays 4.5 against `Arrive` 3.0 on every map. On a table-free arena `Stale`'s clock restarts on those events,
   `Explore`'s bonuses are the pockets' (the information-gain weighting: a cell behind a narrowing is the hidden one)
   and `FrontierPull` aims at the nearest cluster at a pocket. `Seek.TableTerms` 1 honours the arena's flag (the
   Stockades arenas keep v3's exact terms, so the history reads on); 0 is the ablation.
3. **Inputs.** Map block revision 3 adds eight scalars (the nearest and the largest frontier cluster's bearing and
   distance, the cluster count, the cells searched this episode over the whole map); the coverage block (id 27, 12 x 12
   tiles of 32 yd x known / visited / searched, from new per-tile counters of the mental map) puts the search state of
   the whole dungeon in the observation, not only in the GRU. No wire change: protocol 30 stays.
4. **Revisit** 0.002 a second (Cost, fixed, in the score) on a 2-yd cell stood on earlier this episode, weighed by how
   long ago over 60 s: a fifth of Stale's worst case, a tenth of the clock's; a tie-breaker against pacing stood ground,
   never a reason to stand still.
5. **Movement pacing** (amending 0023 and 0026). Move block revision 6 adds the contact side and blocked share the
   controller's slide already knew, the hold age and the pinned age (M1). The trap drill has its own rung
   (`TrapFromRung` 1, where the pins are), a strafe-out counts (`TrapEscapeTurnDeg` 0), an inside corner and a
   pillar's edge join the jamb, and half the drill's episodes start where a training seat pinned, with the keys it
   held, from a per-stage ring of recorded onsets (M2, M3: a start distribution, never a script). `StepCost` is
   halved to 0.00025 so a pause costs half, and `Circling`'s turn clause holds only while a movement key is held: a
   keys-up spin to look round is free (M4, amending 0023's "not turn in circles" order; `turn_in_place_share` is the
   guard). `Recovered` (Aid, outside the score, fades at `AidUntil`) pays back half a pin's Stuck and Wall charges, at
   most 0.3, once the seat is 4 yd away within 10 s: a rebate, never a bonus, so a pin never pays (M5).
   `ExploreFromRung` 1: a camera sweep pays from the doorway rung, where the object is in a cell, not in sight of the
   spawn (M6, amending 0026 item 4). The pacing telemetry reads the controller's body, never the server's position
   (M7). Not done: pricing control churn or course kinks (the classes that steer most find most).
6. **Fresh from M1.** The live v3 run was at the doorway rung; nothing worth carrying. Synthetic layouts (the owner's
   item 8) are out of this rebuild: feasible as a stream of their own (the contract's sec 9).

**Prices.** New terms `frontier_clear`, `pocket_entry` (Exploring), `revisit` (Cost, noise, fixed), `recovered`
(Aid). Keys and defaults in `worldserver.conf.dist` (`Seek.TableTerms` 1, `FrontierClear` 0.1, `ClearMinCells` 3,
`ClearRadius` 36, `PocketEntry` 0.3, `PocketMemoryMs` 10000, `PocketMinCells` 6, `ClusterMinCells` 2, `Revisit`
0.002, `RevisitAgeMs` 60000, `TrapFromRung` 1, `TrapReplayShare` 0.5, `RecoverYards` 4, `RecoverMs` 10000,
`RecoverShare` 0.5, `RecoverCap` 0.3; `StepCost` 0.00025, `TrapEscapeTurnDeg` 0, `ExploreFromRung` 1).

**What it constrains / risks.** (a) The replay table records every unseeded training env's pins, not the greedy
envs' alone: the sim is not told which envs act greedily. (b) Ragefire's cave rooms are convex flat chunks with wide
openings: the jamb pose rarely fits there, `DoorwaySpot` may land outside a chunk, and `HardRoomWeight`,
`BackRoomName` and `RoomEntryBackMult` are inert off the Stockades by name. (c) Pockets are "a chamber behind a
narrowing", not "reachable through one opening". (d) The `arena` column's numbering moved (`sweep` 2, `sweep_rotating`
3): the archived run's numeric column does not line up; the named columns do. (e) The seeded round-robin draw applies
to every stage's seeded, unpinned episodes (M4's two maps alternate by seed now). (f) The Exploring ceiling stays 4.5
at the floor; the first thing to lower is `RoomEntryCap`. (g) Nothing ran: syntax-checked only.

See also [0019](0019-vision-only-movement.md), [0023](0023-exploration-floor.md),
[0025](0025-exploration-v2.md), [0026](0026-searched-state.md) and the [index](README.md).
