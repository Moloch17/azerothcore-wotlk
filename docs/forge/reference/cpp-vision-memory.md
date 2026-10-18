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
  (`map id=22 rev=1 obs=4` when the pin was removed).
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

**Tile counters** (general search, decision 0027, 2026-10-10): a `Tile` also carries `uint16 KnownCells`,
`VisitedCells`, `SearchedCells` and `SearchedEpoch`. `MarkSeen(cell, stamp)` (every look; `WriteEntity` goes through it
now) counts a `Seen 0 -> stamp` transition as known and a `Seen < epoch -> >= epoch` one as searched (restarting the
count when `SearchedEpoch != _epoch`, and adding to the map's `_searchedTotal`); `NoteGround` counts a first
`MAP_VISITED`. Both rely on the cell being the one `Touch` returned last (its tile is `_lastTile`), which every write
path honours. Coarse tiles have no counters (`Fold` keeps them out). Readers: `TileCounts(tx, ty, known, visited,
searched)` (zeros for a missing tile, `searched` 0 for another epoch's count) for the coverage block, and
`SearchedCells()` (reset by `BeginEpisode` / `Clear`) for the map block's `searched_cells` scalar.

### Writes

`WriteFrame(hits, feetZ, bodyHeight)` (:379), per cast ray (at the cast size, before upscaling):
1. "Seen free": a 1-yd 2D grid step from the camera to the hit (or `min(distance, 64)`); the hit's own cell is left to the hit (loop breaks there); each other cell takes the ray's stretch above it and is marked FREE (and `Seen`)
   only if that stretch lies in the body's band: with floor layers known, `low >= floor-0.25 && high <= floor + bodyHeight` for either layer; with no floor known, within `FREE_BAND` (2 yd) of the feet. A pass-through never marks a floor.
2. If the ray hit something within 64 yd (not sky): Terrain/Model with normal z >= 0.7 writes a floor at the hit height (`WriteFloor`: `InsertFloor` merges within 1.5 yd of a layer, else uses a free layer, else replaces the nearer; sets EDGE on the cell and a neighbour whose nearest floor differs by more than
   1 yd); normal z <= -0.3 (ceiling seen from under) writes nothing; in between is a wall: the reference floor is this cell's highest layer at or below `hitZ + 0.5`, else a neighbour's, else the feet; it is marked only when the hit lies within `reference + bodyHeight + 0.5` (or within `[feet-2, feet+body+0.5]` with no reference);
   Water/Deadly mark HAZARD; Door and every other entity class (`class >= Door` except those handled above: `IsEntityClass`) write the class (and DOOR for a door) with a fresh age.
   Entity sensing: units and objects are no longer in the image, so only Door reaches the entity branch from the rays (and a `GroundHazard` ray, a hazard painted on the floor: `FrameHits` carries the class as the pixel has it, so the hazard's area is marked as an entity class, as before). `WriteEntities(SeenList)` then writes, for each listed entity whose class is an entity class, the cell at its middle with its class (`WriteEntity`), and `MAP_DOOR` for a door (open or closed): one loop of at most 32 cells, called from `MapBlock::Observe` right after `WriteFrame` (the entities block precedes the map block in every layout). Rays no longer stop at a unit, so the map sees the wall behind it.
`WriteBody(x,y,z,grounded)` (:534): marks the cells on the line from the last body position (when within 8 yd, `BODY_STEP_MAX`, half-yard steps) and the current cell VISITED and seen; if grounded inserts the feet height as a floor.

### The crop (read) - byte-exact

`Crop(x, y, z, yaw, out)` (:565) writes `CROP x CROP = 48 x 48` cells of 2 yd, heading-up: row 0 is the furthest ahead, column 0 the furthest left, the body at the centre. Cell `(row, col)` centre: `forward = (23.5 - row) * 2` yd, `right = (col + 0.5 - 24) * 2` yd (so row 0 is 47 yd ahead,
col 0 is 47 yd left; the crop spans 96 yd). Each crop cell summarises four 1-yd map cells sampled at `+-0.5` yd round its centre, turned by yaw (forward = (cos, sin), right = (sin, -cos)). `CROP_BYTES = 48 * 48 * 8 = 18432` (7 channels, 16128, until map block revision 6; 6 channels, 13824, until decision 0026). Layout `[row][col][channel]`, 8 bytes a cell:

| channel | byte | value |
|---|---|---|
| 0 CODE | `MapCode`: 0 unknown, 1 floor, 2 wall, 3 door, 4 hazard | wall when at least `CROP_WALL_MIN` 3 of the four cells are walls; else the other cells' codes by priority door > hazard > floor > unknown (`PRIORITY {0,1,4,3,2}` over the non-wall samples). Until revision 6 any wall sub-cell made the cell a wall, which sealed a 4-yd corridor whose wall-footed cells straddle the crop's cells (memv4, 2026-10-17). A cell's code (`CodeOf`) is wall if the wall flag of the floor layer nearest the feet is set (either flag when no floor), else door, else hazard, else floor if any layer, else unknown |
| 1 HEIGHT | 0 no floor, else `clamp(128 + round((floorZ - feetZ)/0.25), 1, 255)` | the floor layer nearest the feet among the four cells (nearest to `z`) |
| 2 VISITED | 0/1 | any of the four VISITED |
| 3 AGE | 255 never; else `min(254, round(20 * log2(1 + s)))` | the newest look among the four, s = `clock - (Seen - 1)` seconds |
| 4 CLASS | 0 none, else class 1..23 | the most recent entity among the four (by `EntityAge`) |
| 5 FRONTIER | 0/1 | any of the four cells is a floor with a 4-neighbour never seen (`Seen == 0`) |
| 6 SEARCHED | 0..4 (`SEARCHED_MAX`) | how many of the four cells have `Seen >= _epoch`: looked at THIS episode (any `MarkSeen` / `WriteEntity`: a ray hit or crossed it, or the body stood on it) |
| 7 WALLS | 0..4 (`CROP_WALLS_MAX`) | how many of the four cells are walls (`CodeOf` wall); revision 6. The coarse crop scales its 64 to the same range: `round(walls x 4 / 64)` |

**Episode epoch and ground counters** (exploration v3, decision 0026). `MentalMap` holds `_epoch` (the first `Stamp()` of the episode; 1 after `Clear()`), `_groundTotal` and `_lastGroundClock`. `BeginEpisode()` (after `Advance` / `Clear`, `StageScenario::ObserveSeat`'s map reset) sets `_epoch = Stamp()` and zeroes the ground counters. The kept-map path advances by `max(MapAgeOffset, 1.0f)` first, because `Stamp()` is `floor(clock) + 1` (1 s resolution): an offset under a second would leave the last stamp of the episode before equal to the new epoch. Every look of an earlier episode then has `Seen < _epoch` strictly. The writers are unchanged; `Crop` compares the stored stamps with the epoch (one compare and add per sub-cell visited). The only counters are in `WriteBody`, via `NoteGround`: a cell the body first stands on (`MAP_VISITED` not yet set, in the interpolated cells of a fast step and the body's own cell) adds one to `GroundTotal()` and resets `SecondsSinceGround()`; exact in an evaluation, an under-count in the 50% kept-map training episodes (`MAP_VISITED` outlives the episode, as the `visited` channel does). Looks are not counted for the scalars on purpose: they accrue at 1,600+ cells an episode and never go quiet; first stands go quiet when the seat re-walks its own ground (the hub shuttle). Constants `GROUND_AGE_SCALE_S` 120 and `GROUND_TOTAL_SCALE` 3000 normalise the two map-block scalars.

`DecodeCropCell` (:166) reads them back as the learner does (height `(b-128)*0.25`, age `2^(b/20) - 1`, `Searched` clamped to 4). The crop first copies a window of `(2 * 70 + 1)^2 = 141 x 141` map cells (radius `ceil(48 * 2 * 0.5 * 1.41422) + 2 = 70`) into a thread-local buffer (19881 cells, about 159 KB), reading coarse cells where no fine tile exists, then samples it; per decision per seat this is the
crop's cost besides `WriteFrame` (up to 64 grid steps per ray, about 2056 rays) and the linear `Evict` scan when full. The Python decode (`mappo/networks.py:1137`) uses height `(b-128)/127`, age `b/255`, "known" = height byte != 0 (differs from the C++ scalar, see issues).

`MapColour` / `MapPanel` (FrameImage.cpp) draw the crop for the audit; see cpp-vision-video.md.

**The coarse crop** (`CropWide`, map block revision 5: the same bytes at `COARSE_CELL` 8 yd, 384 yd across) scatters the
fine tiles' cells by their centres and fills what they leave unknown from the folded (`_coarse`) and far (`_far`, hits beyond
`WRITE_REACH`) tiers' 8-yd cells. Since 2026-10-17 a far or folded cell with a floor is read only when that floor is within
`COARSE_READ_BAND` 6 yd of the feet (`NearestFloor`); a cell with no floor (a wall or hazard alone) is read as it is. Before
that, far-tier floors at any height -- the sea under the Deadmines' cliffs, the hills over them, other bots' storeys from the
shared memory -- were 47-74 % of a memory-on coarse crop, painted as floor outside the dungeon's shell. The shared memory
still adopts far tiles (`SharedMemory::Adopt`): the band at read time covers the seat's own far sightings and adopted ones
alike. A coarse cell's code is a wall only when more than half of its 1-yd cells are (revision 6; a thin wall across an
8-yd cell is a sixteenth of it), else the plurality of the other codes (unseen cells counted as unknown; a tie to the
code the fine crop ranks higher).

**The route on the map** (`MapPlanner.h`, map block revision 7, 2026-10-17; `MapBlock::Observe`): when the
encounter names an objective (`SeatView::HasObjective`, not withheld -- the sim's knowledge of the goal, never the
bot's: since revision 7 the nav stages carry no compass and no observation of the straight line to it), the map block
plans the route to it on the map itself -- `PlanOnMap`: a 96 x 96 window of 1-yd cells, axis-aligned and centred on
the body (`MentalMap::Window`, the fine crop's extent), each cell priced by `CodeOf` for the feet (`PLAN_COSTS` unknown
`PLAN_UNKNOWN` 2 -- 1.5 before 2026-10-18 --, floor and door 1, wall 100, hazard 5; a floor or door whose layer nearest the feet is more than `PLAN_HEIGHT_BAND`
3 yd off them priced as unknown), then shaped into **the body's configuration space** (`ConfigurationSpace`): a
non-wall cell whose two opposite neighbours are walls -- N and S, E and W, or both cells of either diagonal pair -- is a
wall (a 1-yd gap is narrower than the body; judged on the walls as read, so a 2-yd passage stays open), and a passable
cell 8-adjacent to a wall (after the closing; the window's edge is no wall) is priced at least `PLAN_INFLATED` 1.3, and
one of those with a wall within two cells on both sides of an axis or a diagonal (a 2-yd passage) `PLAN_PINCHED` 1.6
(3 and 3 before 2026-10-18: at 3 against unknown 1.5 every remembered corridor cost twice the unseen ground beside it,
and the plan took the straight line through the unknown -- 87 % of the failed trips' 10-yd points landed on true
walls; now floor 1 <= inflated 1.3 <= pinched 1.6 < unknown 2, a `static_assert`, the constants in `MapPlanner.h` for
the global planner to share), so a
route runs down the middle of a corridor and a 2- or 3-yd passage is dear but routable (a hazard keeps its 5). The
coarse plan is not shaped: its cells are 8 yd (a cell between two wall cells is a corridor), and in a dungeon nearly
every coarse cell touches a wall cell, so inflation would triple the plane. Then an 8-neighbour min-plus (a step's
yards x the mean of the two cells' factors) settled exactly by Dijkstra; the goal's own cell at 0 when it lies in the
window, and the coarse crop's plan (the learner's rule over the 8-yd bytes, as before) sampled bilinearly at the
centres of the cells just past the window's border, each turned into the crop's frame, as the ring. The search stops
once the cells round the body and the crop before's pose are settled (a steepest-descent walk from a settled cell only
reaches settled cells). A wall is priced, not impassable: where no real route exists the plan's value is the one
through a wall at a hundred yards a cell, and the walk takes it. Planned over the 2-yd crop, the shared memory's route
read 2-3 x the bake's and 12-18 % unreachable; on the 1-yd cells the same memory's ratio is 1.01 (memv4), 1.03-1.06
with the inflation (every door's cells are dear).

Outputs: the map block's seventeen route scalars, appended last in this order -- `route_p5_sin, route_p5_cos,
route_p10_sin, route_p10_cos, route_p20_sin, route_p20_cos, route_p30_sin, route_p30_cos, route_fine_reach,
route_fine_valid, route_coarse_sin, route_coarse_cos, route_coarse_dist, route_coarse_valid, route_dist,
route_dist_valid, route_stall`. `FineRoute`: the learner's `descend` from the body's cell (from the lowest unblocked
cell round it when its own is a wall's foot), up to `PLAN_FINE_STEPS` 30 steps of a cell (a diagonal step counts one,
as the learner's), the cell reached after 5, 10, 20 and 30 steps (`PLAN_ROUTE_STEPS`) each as sin = -right / d, cos =
forward / d in the body's heading-up frame; a walk that ends earlier (the goal reached, no lower neighbour) repeats
its end cell for the later points; `route_fine_reach` = the steps it made / 30; `route_fine_valid` 1 when it moved and
an objective is named, every fine value 0 otherwise. `CoarseWaypoint` as before: from the learner's anchor cell (23,
23) on the coarse plan, 2 steps of 8 yd, sin, cos, dist = d / 16 (past 1 on a diagonal), valid. `route_dist` =
min(C_map(body), 300) / 300 with `route_dist_valid` 1 when the body's own-map cost-to-go is reachable, else both 0
(`PLAN_DIST_SCALE`). `route_stall` (`RouteStall`; the clock on `CropPose::RouteBest/RouteStall`, carried from one
decision's pose to the next and reset with the pose at an episode's start): seconds since the body's own-map cost-to-go
last fell below its least so far this episode, over `PLAN_STALL_SECONDS` 30, clamped to 1 -- 0 at the first decision,
reset by a new least, running while the cost is unreachable; not advanced on a decision with no objective. The bot's
own map throughout: non-privileged. The plan's value at the body and at the crop before's pose are carried on
`CropPose::Route*` to the nav labels' `[2]` / `[3]` (cpp-encounters.md). The manifest says `route_points_yards [5, 10,
20, 30]`, `route_fine_reach_yards 30`, `route_coarse_reach_yards 16`, `route_dist_yards 300`, `route_stall_seconds 30`.
**The route kept** (`Vision::KeepRoute`, 2026-10-18; `PointNav.RouteKeep` 1, `RouteKeepShare` 0.1, `RouteKeepYards`
5, set once by the scenario through `MapBlock::ConfigureRoute`, recorded in the manifest as `route_keep*`): the fine
walk a decision followed is carried on `CropPose::Kept` (world 1-yd cells, the start and up to 30 steps) and followed
again the next decision: joined from the body's start cell at its furthest-along cell within `PLAN_KEEP_OFF` 2 cells
(two off only past an open middle cell), trimmed behind the join, priced on the current plan (each step's yards x the
mean of its cells' factors, the Dijkstra's rule, plus the current value at its end, which `PlanOnMap` settles as a
`near` point) and extended downhill on the current plan to 30 steps. The new plan's own walk (`FineWalk`) replaces it
when its value at the start undercuts the kept cost by more than 10 % of it and 5 yd both, when a kept cell is now
blocked, outside the window or its end unreached, or when the body is off it. The eight fine scalars (`route_p5_sin`
.. `route_fine_valid`, `WalkRoute`) come from the walk followed and `route_dist` is its cost while kept (the plan's
value at the body otherwise); `route_stall`, the coarse waypoint and the labels' `[2]` / `[3]` (`PlanValueAt`) are the
plan's as before. `CropPose::RouteSwitches` counts the decisions whose followed walk was replaced by one leaving it (a
cell of the new walk, as far as the old reaches, more than 2 cells from every cell of the old; a body that left the
walk is not counted): PointNav's `route_switches` column. `RouteKeep` 0 is the old behaviour (`FineRoute` is
`WalkRoute(FineWalk)`, byte-identical on tmp/mapfix/check). Harness (tmp/keep/keep.cpp, 2026-10-18): a body dithering
0.6-1.2 yd on the line of symmetry before a 7-yd pillar flips the walk's side 9-11 times in 40 decisions without it, 1
with it (0 switches); a wall dropped across the kept door switches at once (1 switch, either setting); a kept walk
followed on an unchanged map prices within 1-2 % of the plan's value. Cost: no measurable change (330-370 us a
decision with the goal ahead, 820-930 us 150 yd behind, old and new alike; the kept end settles before the body).
An L-shaped known corridor whose inner wall is unseen (tmp/keep/corridor.cpp): 6 cells of the walk cut through the
unknown corner at the old costs, 1 at the new; a seen 2-yd L corridor read 231 yd (old) and 123 yd (new) for an 80-yd
route.

Cost on a synthetic room map (tmp/mapfix/check, 2026-10-17): 350-360 us a plan with the goal ahead (325-355 before the
configuration space), 790 us with it 150 yd behind (820 before; most of the window settles before the body), 50-60 us
with no coarse crop (30-40 before); the block's `map` cost row includes it. Planned once a decision a seat.

**Hazards at the frame's end** (`NoteHazard`, `SettleFrame`; memory version 5): a ray's own hit on water, magma or
slime (within `WRITE_REACH` a 1-yd cell, beyond it a far cell) is written after the frame's floors, and not at all
when the cell's floor nearest the hit stands above it by more than `FLOOR_STEP` -- the mesh's guard for a hazard
triangle applied to the direct hits too. The caster takes a WMO's liquid plane wherever a solid is not nearer
(`VisionCaster.cpp` step 2 has no ground-below-the-plane test, unlike the terrain's at :557), so a ray through a gap in
the collision, or along the shore's dry edge, hits the plane under a dry floor. Measured (tmp/mapfix/shore, 40 memv4
"false hazard" poses a map, 8 frames each): the guard drops 3 % of the Deadmines' direct-hit hazard cells (895 of
28,934; 0.5 % of its liquid hits land under dry solid) and none of Shadowfang's -- and at 21 of the 40 Deadmines
poses and 39 of the 40 Shadowfang ones the body stands in water, so most of the 6-8 % of bake-Free floor that read
hazard in the version 4 memory is the waterline the bake calls Free (it has no Water class in either map), not a
writer. A hazard hit on a cell whose floor is not yet known is still written (the cell has no liquid level to judge by
later).

### Invariants

- Written only from camera rays and the body; never from the navmesh or the map data (an unseen cell is unknown whatever is there). Never written in an evaluation across resets (evaluations start empty).
- `Seen` is a 16-bit second stamp; entity age is re-expressed against the newest look (`MarkSeen`, `Merge`), so it is approximate (bucket start).
- Not thread safe; owned by one seat, used on its map thread.
- Shared memory (`SharedMemory.h`, `MEMORY_VERSION` 5 since 2026-10-17; a version 4 file is not read): a blank
  (anchored, not adopted) seat's merge clears a wall the memory holds on a layer both hold when the seat's look is
  newer and its cell carries `MAP_FREE` (a ray passed it in the body's band: `WallClears::SeenFree`); an adopted seat
  clears any wall it lacks on a newer look (`Lacking`); the far tier never clears (`Never`). An older look changes
  nothing. `MAP_FREE` is any in-band pass, head height included -- looser than the map's own clear (`FREE_CLEAR_RISE`,
  `FREE_CLEAR_SPAN`), and `MergeTile` strips `MAP_VISITED` before merging, so standing there is not evidence here.

### Tests

No tests (removed 2026-10-07); see [tests.md](tests.md).

## EntityMemory

In-process only: `Remembered` is never serialized (a grep of `Vision/` finds no serialization; UNVERIFIED: nothing else in the tree persists it, the sight block reads it through `Find/Recall` each decision). Fields in order (EntityMemory.h:61): `u64 Guid` (0 = free entry), `u16 MemoryId` (index + 1), `Identity Id`, `u32 Entry`, `bool GameObject`, `float Level`,
`Health`, `i8 Reaction`, `bool Dead`, `Open`, `Used`, `Vec3 Position` (its middle, as seen), `float Radius`, `float Height` (its size, as seen; the recalled slot's angular size), `float Heading`, `Vec3 Velocity`, `bool Moving`, `double LastSeen`, `u64 LastWrite`.

- Written only from sight: `Write(SeenList)` (cpp:107) records each listed entity (made on first sight), the rest keep their last-seen values (alive stays alive until seen dead). Every `Write` counts, even with nothing listed.
- Ids: the entry's index + 1, stable while remembered; `Configure` with a different cap forgets everything (ids are places) but keeps the clock; full: `Make` evicts the entry with the smallest `LastSeen` among those not shown by this write (ties: lowest index), and its id is reused. `Make` can return null only if every entry was shown by this write (needs cap < listed count; unreachable at cap 64 with 32 slots).
- Course: a known entity seen again within `VELOCITY_WINDOW 2 s` and by the immediately previous write (`LastWrite + 1 >= _writes`) gets `Velocity = (centre - previous)/dt` and `Moving = horizontal speed >= 0.3 yd/s`; if it was not seen in the previous write the old Velocity/Moving are kept stale while the position updates (cpp:125).
- `Irrelevance` (:153) = `log2(1 + age) + distance/20`, minus 2 (quest), minus 1 (usable or door), minus 1 (living hostile); `Recall(from, out, count)` returns the `count` lowest-scoring entries not visible now (`partial_sort`, ties by id). Eviction is by age alone; relevance orders only the recall.
- Cost: `Slot`, `Find`, `Make` are linear scans over the cap (64) per listed entity; `Recall` ranks all entries.
- Lifetime: as the map (above). The sight block reads ids as `ENTITY_MEMORY` (u16 cast to float) and `memory_ids` in the manifest is `MEMORY_TRAINING_CAP`.

`InstanceEncounter.cpp:2464` iterates `Recall.Entries()` (encounter logic reading memory; see cpp-encounters.md).

## ObjectMemory (Vision/ObjectMemory.*, 2026-10-18)

"I want it to remember objects it has seen so it knows where to find them." Entity marks in the mental map are one
class + age byte a cell, which the shared memory drops on merge and the crop shows only under 60 s old; `EntityMemory`
(above) lives one seat's episode. `ObjectMemory` is the lasting list: per seat (`SeatState::Objects`, in a stage with a
map or sight block), per map id shared (`SharedMemory::Objects()`, a `SharedObjects`), saved to disk.

**Perception-true**: the one write is `EntitiesBlock::Observe` (after the entity memory's): `Advance(DecisionMs)` then
`Write(*view.Seen, LookOf(seen, settings), view.Hits)`. Nothing else writes it; nothing from the server's object lists.

**Identity** (`KeyOf`): `EntityInfo::SpawnId` (new field, filled in `MapVisionWorld.cpp` `ClassifySeen` from
`Creature::GetSpawnId` / `GameObject::GetSpawnId`: the DB guid, the same in every instance and after a restart) as
`ObjectSource::CreatureSpawn` / `ObjectSpawn`; else the raw GUID (`Guid`: players, summons, temporary objects; never
persisted). Ground hazards are not kept.

**Entry** (`RememberedObject`): key, template entry, class at the last sighting, kind (`ObjectKind`: `Fixed` every game
object -- door, gate, lever, chest, mailbox, node; `Npc` a unit of class quest giver / vendor / trainer, or usable and
not hostile -- vendor, trainer, flight master; sticky once set; `Creature`; `Player` -- `KindOf`), state as seen (`Open`,
`Used`, `Dead`; the seeing seat's `Quest`, `Lootable`, `Usable`), `Absent` + `Missed`, reaction, level, health, middle
(x, y, z), facing, radius, height, `FirstSeen` / `LastSeen` / `LastLook` (memory seconds, double), `Sightings`, and a
trace of `OBJECT_TRACE` 8 points (creatures and players: a point when it moved `TRACE_STEP` 2 yd from the last).

**Rules**: a sighting updates in place (fields as seen, Sightings + 1, Missed 0, Absent off). **Expected but unseen**:
an entry not listed whose middle is within `LOOK_REACH` 40 yd of the camera (and the frame's range; nearer than the
list's farthest entry when the list is full at 32), inside the frame's field of view (`look.FovH/FovV`), and not behind
the ray nearest its direction (`FrameHits`, found by hill-climbing the rays' own directions from the pixel the angles
name; blocked when the ray stops more than radius + `LOOK_SLACK` 1 yd short; sky never blocks) is a missed look;
`MISSED_LOOKS` 4 in a row set Absent (a gathered node, a looted chest, a moved NPC, a despawned corpse). No rays (a frame
without an image): nothing is judged. **Decay** (every `DECAY_EVERY_SECONDS` 5): players after `PLAYER_KEEP_SECONDS`
120 s unseen, any other GUID-keyed thing after `TRANSIENT_KEEP_SECONDS` 600 s. Spawned creatures are kept like fixed
objects and NPCs (the spawn point and the patrol trace); their age says how stale the state is. **Cap**
`OBJECT_SEAT_CAP` 1024 a seat, `OBJECT_SHARED_CAP` 8192 a map: full, the entry not listed by this write that matters
least goes (GUID-keyed / players, then creatures, then objects and NPCs; oldest sighting first).

**Sharing** (`SharedObjects`, its own mutex): in a stage with `SharedMemoryShare`, at the episode's first observation
(`StageScenario::ObserveSeat`, beside the terrain's): the episode before's memory is absorbed when anchored and
`MapAbsorb` (training only: evaluations never write), then the seat adopts (`MapShared`, the same draw as the terrain's;
never under the `no_object_memory` arm) or anchors blank. Absorb merges the entries looked at since anchoring, their
times moved onto the store's clock (the seat's newest look reads as now), by identity: the newer `LastSeen` decides the
place, state, class and trace (`Sightings` the larger, `FirstSeen` the earlier, an NPC stays one); the newer `LastLook`
decides Missed/Absent. Seat facts are stripped on the way in (quest giver -> usable object / friendly creature, lootable
corpse -> corpse, quest object -> usable object, `Quest`/`Lootable` off). Without a shared memory the seat's memory is
kept by the map's roll (`MapKeep`, aged `MapAgeOffset`) on the same map id (any instance: spawn ids), else cleared.

**Persistence**: `<DataDir>/memory/<mapId>.aobj` beside the `.amem` (its own format, so `MEMORY_VERSION` 5 is
unchanged), loaded in `SharedMemories::Get`, written in `SaveList` (periodic saver thread and shutdown) when
`SharedObjects::Dirty`. `ObjectFileHeader` 64 bytes: magic `AOBJ`, `OBJECT_MEMORY_VERSION` 1, map id, record bytes 176,
clock (double), wall seconds, count, FNV-1a 64 of the records, reserved. Then `ObjectRecord`s (176 bytes, packed: source,
kind, class, flags `OBJECT_FLAG_*` (game object, open, used, dead, absent, usable), id, entry, reaction, missed, trace
count, level, health, xyz, facing, radius, height, first/last seen, last look, sightings, 8 x xyz trace), lasting keys
only (no GUIDs, no players), in identity order. A file of another version, map or record size, or a bad checksum, is
refused (logged) and overwritten at the next save. The clock carries on from the save plus the wall-clock downtime.

**Planner API** (no caller yet): `Find(kind or ObjectKind::Count, class or Class::Count, near, radius, out)` the
matches nearest first (Absent included); `BlockingAt(cx, cy)` -- a remembered closed door/gate (class Door, game object,
not Open, not Absent) whose middle is within max(radius, `DOOR_MIN_RADIUS` 1) of the 1-yd cell's centre: a wall to a
route until a look sees it open (the global planner's contradiction replanning). Linear over the entries.

**The policy's view**: the `memory_entities` block (cpp-blocks.md). **The arm**: `no_object_memory`
(`MODE_FLAG_NO_OBJECT_MEMORY` 16; `SeatState::ObjectsHidden`, set at the reset for an evaluation): no adoption, no write,
the block reads zeros.

Cost: a write is one hash lookup a listed entity plus one pass over the entries (frustum test; a hill climb only for
the few in view); `Recall` ranks all entries (partial sort). Harness: `tmp/objmem/check.cpp` (identity merge, decay,
expected-but-unseen incl. occlusion / frustum / full list / no rays, cap order, shared newest-wins, adopt / absorb,
`.aobj` round trip, other-map and corrupt files refused): all pass.

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
