# Vision: the mental map and entity memory (Vision/MentalMap.*, Vision/EntityMemory.*)

Read from `forge` bd32b9dc8; paths relative to `src/server/game/Animus/`. Part of the Vision reference: see [cpp-vision.md](cpp-vision.md)
(camera, caster, map table) and [cpp-vision-video.md](cpp-vision-video.md). Consumers (MapBlock, EntitiesBlock, SightBlock) are in
[cpp-blocks.md](cpp-blocks.md); wire layout in [protocol.md](protocol.md); Python decode in [py-mappo.md](py-mappo.md).

## Map table

| path | lines | role |
|---|---|---|
| Vision/MentalMap.h | 307 | Cell/tile/crop types and constants, `MentalMap` class, settings. |
| Vision/MentalMap.cpp | 678 | Writes from a frame's rays and the body, eviction/coarse folding, the heading-up crop. |
| Vision/EntityMemory.h | 130 | `Remembered`, `EntityMemory`, caps. |
| Vision/EntityMemory.cpp | 182 | Write from a seen list, ids, recall ranking. |

## Where they live and what drives them

Both are per-seat members of `SeatState` (`Scenario/Curriculum/StageState.h:206` `Map`, `:217` `Recall`), written only from the seat's own sight:
- `MapBlock::Observe` (`Blocks/MapBlock.cpp:~101`): `map->Advance(decisionSeconds)`, `WriteFrame(*view.Hits, z, bodyHeight)` (when the vision block left rays), `WriteEntities(*view.Seen)` (the entities the sensor listed; entity sensing), `WriteBody(x,y,z,grounded)`,
  then `Crop(x, y, z, facing, row)` into the STEP's map section; the block's 4 float columns are known/frontier/visited share of the crop's cells and a "kept" flag
  (golden: `map id=22 rev=1 obs=4`).
- `EntitiesBlock::Observe` (`Blocks/EntitiesBlock.cpp:~76`): `memory->Advance(decisionSeconds)` then `memory->Write(*view.Seen)` (the only write), only if the seat has a `SeenList`
  (when `view.Seen` is null it returns before touching memory, so memory's clock does not advance that decision). `SightBlock::Write` reads it (`Find`, `Recall`).
- Persistence across resets (`StageScenario.cpp:1936-1943` and `:3570-3590`): in a stage with a map or sight block, each reset rolls `MapKeep` (training only, never an evaluation) with probability
  `AnimusForge.Map.KeepShare` and a random age offset in `[0, AgeOffsetSeconds]`; at the episode's first observation, if the seat is on the same map id and instance id and the map/memory is not empty it is kept and
  `Advance(offset)` ages it, else `Clear()`; then `Configure(caps)`. Memory uses the same roll and offset. `Look`-style reset is in `ResetEpisode`; the map/memory are NOT reset there (they are kept members).

Config keys (ForgeConfig.cpp:316-325; conf dist `worldserver.conf.dist:5454-5497`): `AnimusForge.Map.MaxTiles` 4096 (1..65536), `AnimusForge.Map.CoarseTiles` 0 (0..65536), `AnimusForge.Map.KeepShare` 0.5 (0..1),
`AnimusForge.Map.AgeOffsetSeconds` 600 (0..36000), `AnimusForge.Memory.MaxEntities` 64 (1..1024). Training uses the 64 cap (`MEMORY_TRAINING_CAP`); a larger cap is for a realm bot. See config-keys.md.

## MentalMap

### Storage layout (byte-exact)

World-anchored cells of 1 yd (`MAP_CELL`); cell `(cx, cy) = floor(x), floor(y)`. Tiles of 32 x 32 cells (`MAP_TILE`); tile key `(int64(tx) << 32) ^ uint32(ty)` (MentalMap.h:270) in `unordered_map<int64, unique_ptr<Tile>>`; a tile is
`std::array<MapCell, 1024>` plus `uint64 Touched` (8192 bytes of cells). A `MapCell` is 8 bytes (`static_assert`, MentalMap.h:100):

| offset | field | meaning |
|---|---|---|
| 0 | `int16 Floor[0]` | lower floor layer, in quarter yards (`QuantiseFloor = round(z/0.25)`); `INT16_MIN` = none |
| 2 | `int16 Floor[1]` | upper layer, same units; layers are kept sorted lower-first (`SortLayers` also swaps the wall flags) |
| 4 | `uint8 Flags` | `0x01` WALL_LOW (wall in the band of layer 0, or near the feet when no floor known), `0x02` WALL_HIGH (layer 1), `0x04` DOOR, `0x08` HAZARD (water/magma/slime), `0x10` EDGE (step or drop to a neighbour), `0x20` VISITED (the body stood here), `0x40` FREE (a ray passed in the body's band) |
| 5 | `uint8 Entity` | bits 0-4 class of the entity last seen here (0 none); bits 5-7 age bucket of that entity relative to the cell's last look (`ENTITY_AGE_BUCKETS {0,1,4,15,60,240,900,3600}` s) |
| 6 | `uint16 Seen` | map second of the last look + 1 (`Stamp()` = min(65535, floor(clock)+1)); 0 = never |

Constants: `FLOOR_NORMAL 0.7`, `CEILING_NORMAL -0.3`, `LAYER_MERGE 1.5`, `EDGE_RISE 1.0`, `FREE_BAND 2.0`, `WALL_SLACK 0.5`, `WRITE_REACH 64` yd, `COARSE_CELL 8`. Clock: seconds as `double`; `Advance` clears the whole map when the clock reaches
`CLOCK_LIMIT = 65000` s (MentalMap.cpp:26,228; about 18 h: a long-lived realm bot loses its map). Cap: `MaxTiles`; at the cap `Touch` evicts the least recently touched tile by a linear scan of all tiles (`Evict`, :260), folding it into
coarse 8-yd tiles when `CoarseTiles > 0` (`Fold`, :275: floors as layers, flags OR-ed, newer look and entity kept) and evicting the oldest coarse tile when that cap is reached. With `CoarseTiles = 0` (training) an evicted tile is forgotten.
`FindCell` never reads coarse tiles; only `Crop` does (`FindCoarse`, :323).

### Writes

`WriteFrame(hits, feetZ, bodyHeight)` (:379), per cast ray (at the cast size, before upscaling):
1. "Seen free": a 1-yd 2D grid step from the camera to the hit (or `min(distance, 64)`); the hit's own cell is left to the hit (loop breaks there); each other cell takes the ray's stretch above it and is marked FREE (and `Seen`)
   only if that stretch lies in the body's band: with floor layers known, `low >= floor-0.25 && high <= floor + bodyHeight` for either layer; with no floor known, within `FREE_BAND` (2 yd) of the feet. A pass-through never marks a floor.
2. If the ray hit something within 64 yd (not sky): Terrain/Model with normal z >= 0.7 writes a floor at the hit height (`WriteFloor`: `InsertFloor` merges within 1.5 yd of a layer, else uses a free layer, else replaces the nearer; sets EDGE on the cell and a neighbour whose nearest floor differs by more than
   1 yd); normal z <= -0.3 (ceiling seen from under) writes nothing; in between is a wall: the reference floor is this cell's highest layer at or below `hitZ + 0.5`, else a neighbour's, else the feet; it is marked only when the hit lies within `reference + bodyHeight + 0.5` (or within `[feet-2, feet+body+0.5]` with no reference);
   Water/Deadly mark HAZARD; Door and every other entity class (`class >= Door` except those handled above: `IsEntityClass`) write the class (and DOOR for a door) with a fresh age.
   Entity sensing: units and objects are no longer in the image, so only Door reaches the entity branch from the rays (a `GroundHazard` ray is a floor with a hazard painted on it: `FrameHits` keeps the floor under it and the floor is written). `WriteEntities(SeenList)` then writes, for each listed entity whose class is an entity class, the cell at its middle with its class (`WriteEntity`), and `MAP_DOOR` for a door (open or closed): one loop of at most 32 cells, called from `MapBlock::Observe` right after `WriteFrame` (the entities block precedes the map block in every layout). Rays no longer stop at a unit, so the map sees the wall behind it.
`WriteBody(x,y,z,grounded)` (:534): marks the cells on the line from the last body position (when within 8 yd, `BODY_STEP_MAX`, half-yard steps) and the current cell VISITED and seen; if grounded inserts the feet height as a floor.

### The crop (read) - byte-exact

`Crop(x, y, z, yaw, out)` (:565) writes `CROP x CROP = 48 x 48` cells of 2 yd, heading-up: row 0 is the furthest ahead, column 0 the furthest left, the body at the centre. Cell `(row, col)` centre: `forward = (23.5 - row) * 2` yd, `right = (col + 0.5 - 24) * 2` yd (so row 0 is 47 yd ahead,
col 0 is 47 yd left; the crop spans 96 yd). Each crop cell summarises four 1-yd map cells sampled at `+-0.5` yd round its centre, turned by yaw (forward = (cos, sin), right = (sin, -cos)). `CROP_BYTES = 48 * 48 * 6 = 13824`. Layout `[row][col][channel]`, 6 bytes a cell:

| channel | byte | value |
|---|---|---|
| 0 CODE | `MapCode`: 0 unknown, 1 floor, 2 wall, 3 door, 4 hazard | the four cells' codes by priority wall > door > hazard > floor > unknown (`PRIORITY {0,1,4,3,2}`); a cell's code (`CodeOf`) is wall if the wall flag of the floor layer nearest the feet is set (either flag when no floor), else door, else hazard, else floor if any layer, else unknown |
| 1 HEIGHT | 0 no floor, else `clamp(128 + round((floorZ - feetZ)/0.25), 1, 255)` | the floor layer nearest the feet among the four cells (nearest to `z`) |
| 2 VISITED | 0/1 | any of the four VISITED |
| 3 AGE | 255 never; else `min(254, round(20 * log2(1 + s)))` | the newest look among the four, s = `clock - (Seen - 1)` seconds |
| 4 CLASS | 0 none, else class 1..23 | the most recent entity among the four (by `EntityAge`) |
| 5 FRONTIER | 0/1 | any of the four cells is a floor with a 4-neighbour never seen (`Seen == 0`) |

`DecodeCropCell` (:166) reads them back as the learner does (height `(b-128)*0.25`, age `2^(b/20) - 1`). The crop first copies a window of `(2 * 70 + 1)^2 = 141 x 141` map cells (radius `ceil(48 * 2 * 0.5 * 1.41422) + 2 = 70`) into a thread-local buffer (19881 cells, about 159 KB), reading coarse cells where no fine tile exists, then samples it; per decision per seat this is the
crop's cost besides `WriteFrame` (up to 64 grid steps per ray, about 2056 rays) and the linear `Evict` scan when full. The Python decode (`mappo/networks.py:1137`) uses height `(b-128)/127`, age `b/255`, "known" = height byte != 0 (differs from the C++ scalar, see issues).

`MapColour` / `MapPanel` (FrameImage.cpp) draw the crop for the audit; see cpp-vision-video.md.

### Invariants

- Written only from camera rays and the body; never from the navmesh or the map data (an unseen cell is unknown whatever is there). Never written in an evaluation across resets (evaluations start empty).
- `Seen` is a 16-bit second stamp; entity age is re-expressed against the newest look (`MarkSeen`, `Merge`), so it is approximate (bucket start).
- Not thread safe; owned by one seat, used on its map thread.

### Tests

`MentalMapTest.cpp`: `WritesComeOnlyFromCastPixelsAndTheBody`, `ARayOverAVoidMarksNoFloor`, `FreeAndWallsOnlyInTheBodysBand`, `TwoHeightLayers`, `TheCropTurnsWithTheFacing`, `TheTwoYardCellSummarisesItsFour`, `Frontier`, `PersistenceAndAgeing`, `CapsAndCoarseTiles`, `CropBytesRoundTrip` (:419, the byte contract), `TheMapBlockDescribesItsCrop`; `VisionTest.TheMentalMapReadsTheCastFrame`; golden pin `map id=22 rev=1 obs=4`.

## EntityMemory

In-process only: `Remembered` is never serialized (a grep of `Vision/` finds no serialization; UNVERIFIED: nothing else in the tree persists it, the sight block reads it through `Find/Recall` each decision). Fields in order (EntityMemory.h:61): `u64 Guid` (0 = free entry), `u16 MemoryId` (index + 1), `Identity Id`, `u32 Entry`, `bool GameObject`, `float Level`,
`Health`, `i8 Reaction`, `bool Dead`, `Open`, `Used`, `Vec3 Position` (its middle, as seen), `float Radius`, `float Height` (its size, as seen; the recalled slot's angular size), `float Heading`, `Vec3 Velocity`, `bool Moving`, `double LastSeen`, `u64 LastWrite`.

- Written only from sight: `Write(SeenList)` (cpp:107) records each listed entity (made on first sight), the rest keep their last-seen values (alive stays alive until seen dead). Every `Write` counts, even with nothing listed.
- Ids: the entry's index + 1, stable while remembered; `Configure` with a different cap forgets everything (ids are places) but keeps the clock; full: `Make` evicts the entry with the smallest `LastSeen` among those not shown by this write (ties: lowest index), and its id is reused. `Make` can return null only if every entry was shown by this write (needs cap < listed count; unreachable at cap 64 with 32 slots).
- Course: a known entity seen again within `VELOCITY_WINDOW 2 s` and by the immediately previous write (`LastWrite + 1 >= _writes`) gets `Velocity = (centre - previous)/dt` and `Moving = horizontal speed >= 0.3 yd/s`; if it was not seen in the previous write the old Velocity/Moving are kept stale while the position updates (cpp:125).
- `Irrelevance` (:153) = `log2(1 + age) + distance/20`, minus 2 (quest), minus 1 (usable or door), minus 1 (living hostile); `Recall(from, out, count)` returns the `count` lowest-scoring entries not visible now (`partial_sort`, ties by id). Eviction is by age alone; relevance orders only the recall.
- Cost: `Slot`, `Find`, `Make` are linear scans over the cap (64) per listed entity; `Recall` ranks all entries.
- Lifetime: as the map (above). The sight block reads ids as `ENTITY_MEMORY` (u16 cast to float) and `memory_ids` in the manifest is `MEMORY_TRAINING_CAP`.

Tests: `EntityMemoryTest.cpp` (7): `OnlyWhatTheFrameShowsIsWritten`, `AKillOutOfSightStaysAPlaceUntilTheCorpseIsSeen`, `SightingsAgeWithTheClock`, `MemoryIdsAreStableWhileRemembered`, `TheOldestSightingIsForgottenFirst`, `APatrolsCourseIsRemembered`, `RecallTakesTheMostRelevantUnseen`; `SightBlockTest.cpp` for the columns; `InstanceEncounter.cpp:2464` iterates `Recall.Entries()` (encounter logic reading memory; see cpp-encounters.md).

## Observed issues

1. `MentalMap::Advance` clears the whole map at 65000 s (MentalMap.cpp:228): a realm bot loses all memory after about 18 h.
2. `MentalMap::Evict` is an O(tiles) scan on every new tile at the cap; `EntityMemory` lookups are linear. Fine at 64, scaling poorly at the realm's 1024.
3. `EntityMemory::Write` keeps a stale `Velocity`/`Moving` for an entity that reappears after a gap (cpp:125-128).
4. `MentalMap::Clear` does not reset `_lastKey`; harmless because `_lastTile` is nulled.
5. `MapBlock::Scalars` counts "known" as age != 255 (MapBlock.cpp) but `decode_map` (networks.py:1146) treats a cell as known only when the height byte is nonzero; a seen-free cell with no floor is known to one and not the other.
6. `EntitiesBlock::Observe` skips the memory write and clock advance when the seat has no `SeenList` (a seat without a frame): memory ages only on decisions with a frame.
7. `EntityMemory.h` comment names `MapRunSettings::KeepShare` (correct) but the roll and offset live in `StageScenario.cpp:1936-1943`, not here; `Configure` of a changed cap forgets entries but not the clock.
8. `WriteEntity` overwrites `Entity` with a bucket 0, which is right for a fresh sighting but loses the previous entity's class silently when a different entity class is seen in the same cell.
9. The entity class byte `Entity & 0x1F` limits classes to 32 (same as the pixel); a class above 31 would corrupt the age bucket bits (static contract is `CLASS_LIMIT`).

## Reviewer questions

- Is 16-bit second stamps (18 h) enough for the realm, and should `Clear` on overflow instead rebase the stamps?
- `Crop` copies a 141x141 window although the crop needs 96 x 96 yd rotated; confirm the margin (halfDiagonal + frontier neighbour) before changing the radius.
- The map is kept across a reset with probability 0.5 only on the same instance; a dungeon reset to a new instance id always starts empty.
