# Vision: the camera (src/server/game/Animus/Vision/)

Reference for a manual review, read from `forge` bd32b9dc8 and updated for entity sensing (plan
`.agents/plans/entity-sensing/entity-sensing.PLAN.md`; vision block revision 6, protocol 26). Paths are relative to
`src/server/game/Animus/` unless they start with `src/`. Line numbers are of bd32b9dc8 and are stale in the files entity sensing
touched (Camera.h, VisionCaster.*, MapVisionWorld.*, Identity.h). Parts: this file (camera, caster, free look, classes, entity list),
[cpp-vision-memory.md](cpp-vision-memory.md) (mental map, entity memory), [cpp-vision-video.md](cpp-vision-video.md) (frame
images, evaluation videos). Related: [cpp-movement.md](cpp-movement.md), [cpp-blocks.md](cpp-blocks.md) (VisionBlock,
EntitiesBlock, MapBlock, SightBlock that consume these types), [cpp-runtime.md](cpp-runtime.md),
[01-forge-core-delta.md](01-forge-core-delta.md), [protocol.md](protocol.md), [py-mappo.md](py-mappo.md), [tests.md](tests.md),
[known-issues.md](known-issues.md).

## Purpose and scope

A seat sees through a ray-cast third-person camera (principle 1: only what a player sees). Each decision `VisionBlock::Observe`
(`Runtime/Scenario/Curriculum/Blocks/VisionBlock.cpp`) advances the seat's free-look head, places the camera once (`PlaceCamera`), gathers
the entity candidates round the seat (`GatherCandidates`), senses who is in view (`Sense`), classifies the survivors (`ClassifySeen`), casts the
static world with `Vision::Render`, and leaves: 11 float scalars (the block's columns), a byte image (4 bytes a pixel, 128x64 canonical; **the
static world only**) in the seat's byte row, the entity list (`SeenList`) for the entities and sight blocks, and the cast rays (`FrameHits`)
for the map block. The caster is pure over a `VisionWorld`; `MapVisionWorld` is the live-map implementation. The CPU caster is the only camera
(see "The GPU camera (removed)"). **Who is in view is not a pixel question**: units, objects and pickups are never drawn; the entity sensor
(EntitySensor.h) decides by line of sight (see "The entity sensor").

## Map table (Vision/)

| path | lines | role |
|---|---|---|
| Vision/Camera.h | ~400 | Settings, classes (and `PIXEL_CLASSES`), pixel byte contract, scalars, constants, upscale, render-size draw; header-only except settings storage. |
| Vision/VisionCaster.h | ~290 | `VisionWorld` interface (trees, any-hit, liquids, terrain), `HazardDisc`/`Sight` (the frame's hazards), `Pose`, `CameraState`, `Rig`, `Hit`, caster API, `SegmentBlocked`. |
| Vision/VisionCaster.cpp | ~600 | Ray casting against trees, liquids, terrain cells; hazard painting; `SegmentBlocked`; pixel encode/decode; `Render`; `ParseRenderSizes`; `Configure/Current`. |
| Vision/EntitySensor.h, EntitySensor.cpp | 115, 230 | The entity sensor: candidates, frustum and sample points, shadow rays, ranking (new). Pure over `VisionWorld`. |
| Vision/FreeLook.h | 213 | The look head (yaw/pitch rate, zoom steps, recentre, face) and camera integration. |
| Vision/MapVisionWorld.h | ~120 | `MapVisionWorld : VisionWorld` over a `Map`; `GatheredSight`, `GatherCandidates`, `ClassifySeen`, `FactsOf`, `KillTargets`, `HostileGround`. |
| Vision/MapVisionWorld.cpp | ~440 | Implementations; the candidate gather; the survivors' classification. |
| Vision/Identity.h | ~165 | `EntityFacts`, `Identity`, `EntityInfo` (with `Radius`, `Height`, `Los`), `SeenList`, `EntityMark` (the audit's overlay). |
| Vision/Identity.cpp | 84 | `Classify`: facts to semantic class and flags. |
| Vision/VisionCost.h | ~75 | Process atomics for the vision status row (frame ns, pixel rays, the sensor's shadow rays, map ns, STEP bytes). |
| Vision/MentalMap.h, MentalMap.cpp | 307, 678 | Mental map: see cpp-vision-memory.md. |
| Vision/EntityMemory.h, EntityMemory.cpp | 130, 182 | Entity memory: see cpp-vision-memory.md. |
| Vision/FrameImage.h, FrameImage.cpp | 118, 426 | PNG/APNG writers, panels, composite: see cpp-vision-video.md. |
| Vision/EvalVideo.h, EvalVideo.cpp | 205, 494 | Evaluation video recorder: see cpp-vision-video.md. |

## The pixel: byte-exact (Camera.h, VisionCaster.cpp)

One pixel is 4 bytes (`BYTES_PER_PIXEL`, Camera.h; five until protocol 26). The image is `[row][col][byte]`, row 0 at the top, column 0 at the
left; canonical size 128 columns x 64 rows (`AnimusForge.Vision.Width/Height`), so `ImageBytes = 128 * 64 * 4 = 32768` (40960 at five bytes).
`EncodePixel`:

| byte | meaning | encoding |
|---|---|---|
| 0 | distance | `255` (SKY_BYTE) for sky, else `round(254 * clamp(ln(max(d, 0.25)/0.25) / ln(1000/0.25), 0, 1))`; so 0.25 yd is 0 and 1000 yd and beyond is 254; 255 is sky alone. |
| 1 | height of the hit over the seat's FEET | sky: 128; else `128 + clamp(round((hitZ - feetZ)/0.2), -125, 125)` (so +-25 yd range, 0.2 yd steps). |
| 2 | surface normal z | `round(255 * clamp(nz, 0, 1))`; sky and walls 0. |
| 3 | class and objective | bits 0-4 the class (`Class`; a pixel carries one of `PIXEL_CLASSES`), bit 5 (0x20) objective flag, bits 6-7 reserved (0). |

**Classes a pixel carries** (`PIXEL_CLASSES`, manifest `image.pixel_classes` = `[0,1,2,3,4,5,23]`): sky, terrain, model, door (every closed
collision game object: the dynamic tree's hit), water, deadly, ground hazard. Classes 6-22 (units, quest givers, corpses, chests, herbs, objects)
are in the entity list and the map only; the class table is not renumbered (`CLASSES`, `CLASS_NAMES`, `CLASS_LIMIT` unchanged). A non-door
collision game object (a model chest, a bridge) reads as Door in pixels (decision D3); its identity is in the list. An open door has no collision
model, so it leaves the image altogether and shows in the list (`EntityInfo::Open`). The objective flag is unchanged in definition, but a ray no
longer stops at the objective object, so its segment can be longer and flag a pixel that used to stop at the object's face.

`DecodePixel` returns five floats (`DECODED_VALUES`; distance `b/254` or 1.0 for 255; height `(b-128)/125`; normal `b/255`; class `b & 31`; objective
`(b>>5)&1`). The learner's `decode_image` (`apps/forge/python/animus/mappo/networks.py`) matches it exactly. No-frame rows (director, absent agent, no
map) are `{255,128,0,0}` per pixel (`FillNoFrame`; `NO_FRAME_PIXEL` in protocol.py), not zeros. A sky ray can still carry the objective bit
(`ObjectiveFlag` is tested to the ray's reach).
Wire placement: the image travels in the STEP's image section (`Bridge/Protocol.h`, protocol 26); the block's float columns are the scalars alone;
layout at removal of the pin: `vision id=20 rev=6 obs=11`.

### The 11 scalars (Camera.h:282, written at VisionCaster.cpp:740-756)

yaw_sin, yaw_cos of the camera's yaw offset; pitch `/ (pi/2)`; zoom `/ 12`; boom `/ 12` (the pulled-in distance); pivot height (above the floor
below, over `PIVOT_HEIGHT_SCALE 10`, clamped 0..1; 1 when no floor); underwater (camera Z below a liquid at the camera); airborne (Pose.Airborne);
yaw rate `/ (180 deg/s)`; pitch rate `/ (60 deg/s)`; render width `/ canonical width`.

## Settings, render sizes and the mixed-resolution scheme

`Settings` (Camera.h:48): Width 128, Height 64, FovH 120, FovV 60, Range 100 yd (the radius within which units are gathered, plus the zoom; a ray itself
has no range), Zoom 6, Pitch -15 deg, `RenderSizes` and `RenderWeights`. Config keys (ForgeConfig.cpp:289-313, clamped to the range shown, an out-of-range
value logs an error and is clamped; `conf` defaults in `src/server/apps/worldserver/worldserver.conf.dist:5346-5451`):

| key | default | range |
|---|---|---|
| AnimusForge.Vision.Width | 128 | 8..256 |
| AnimusForge.Vision.Height | 64 | 8..256 |
| AnimusForge.Vision.RenderSizes | "32x16, 48x24, 64x32, 128x64:0.4" | each WxH within 1x1..canonical, optional `:weight` > 0 |
| AnimusForge.Vision.FovH / FovV | 120 / 60 | 30..170 / 20..120 |
| AnimusForge.Vision.Range | 100 | 10..500 |
| AnimusForge.Vision.Zoom | 6 | 0..50 |
| AnimusForge.Vision.Pitch | -15 | -80..80 |
| AnimusForge.Vision.AuditInterval / AuditSeats | 300 / 4 | 0..86400 / 1..64 |
| AnimusForge.Vision.EvalVideos / EvalVideoScale | 8 / 4 | 0..64 / 1..8 |
| AnimusForge.Map.* , AnimusForge.Memory.MaxEntities | see cpp-vision-memory.md | |

Width must be a multiple of 16 and Height a multiple of `Width/16` (the learner cuts a 16-column patch grid: `Patch()`, Camera.h:75); a bad size is only
logged (ForgeConfig.cpp:299-302), not corrected. The learner refuses it from the manifest. See config-keys.md.

Mixed resolutions: at every episode reset each seat in a stage with a vision block draws a render size by weight (`DrawRenderSize`, Camera.h:89,
called from `StageScenario.cpp:1926` with the world thread's random numbers; one entry draws nothing; weights default 1; evaluations draw too).
The default draws 128x64 about 0.4/3.4 = 12% of episodes. `Render` casts at `min(RenderWidth, W) x min(RenderHeight, H)` with the SAME field of view (fewer,
wider rays) and scales up into the canonical image by nearest pixel (`Upscale`, Camera.h:377: canonical (r,c) takes cast pixel `(floor(r*h/H), floor(c*w/W))`,
every byte copied as is, so the objective bit is copied). The render size now changes only the static image: the entity list is geometric and the same at every size. The expected cost is `(512 + 1152 + 2048 + 0.4*8192)/3.4 = 2056` rays a frame at the default
weights. The scalar `render_width` tells the network the size.

## Semantic classes (Camera.h:120)

A fixed table: values never change meaning, new classes take the next free value; the byte has room for 32. The whole table is the entity list's and the
map's; a pixel carries only `PIXEL_CLASSES` (0-5 and 23). Count 24 now:
0 sky, 1 terrain, 2 model (static tree: WMO or M2), 3 door (dynamic-tree object or door/button), 4 water, 5 deadly (magma/slime), 6 hostile_creature,
7 neutral_creature, 8 friendly_creature, 9 hostile_player, 10 friendly_player, 11 quest_giver, 12 vendor, 13 trainer, 14 lootable_corpse, 15 corpse,
16 chest, 17 herb, 18 ore, 19 mailbox, 20 quest_object, 21 usable_object, 22 other_object, 23 ground_hazard (`Class::GroundHazard = 23`, Camera.h:148).
`CLASS_NAMES` (:158) is what the manifest prints. `Kind` (revision 4's 8 coarse kinds) follows from the class (`KindOf`, :178); `Deadly` and
`GroundHazard` both map to `Kind::Deadly`, every game-object class to `Door`. Rule: `Classify` (Identity.cpp:68) decides it from the seat's own client facts
(the UI rule: quest marks, lootable and quest objects are per character). First match wins: unit: dead -> lootable corpse (if this seat may loot) or corpse;
player -> hostile/friendly player; hostile -> hostile creature; quest mark -> quest giver; vendor; trainer; friendly/neutral creature. Game object: door or
button -> door; mailbox; quest mark -> quest giver; chest locked by herbalism -> herb, by mining -> ore; quest relevant -> quest object; chest; usable ->
usable object; else other.

## The caster (VisionCaster.cpp)

Contract (header comment VisionCaster.h): every pixel's ray is cast exactly against what the server has loaded (the static world); nothing is marched,
baked, cached or skipped; a ray ends at its first hit or is sky once it has left the loaded grids. `Render(settings, rig, pose, camera, world, sight,
objective, image, scalars, breakdown, objectiveRadius, hits)` takes the `Rig` the caller placed (one `PlaceCamera` a decision, which the sensor reads too) and
the frame's hazard discs (`sight`). In order:

1. `PlaceCamera`: pivot = feet + `0.9 * BodyHeight` (PIVOT_SHARE); azimuth = pose yaw + camera yaw offset; elevation = camera pitch; zoom > 0 casts ONE ray from the
   pivot backwards (`Nearest`, no liquids) and pulls the camera in to `back.Distance - 0.2` but never nearer than `min(0.3, zoom)`; camera =
   pivot - forward * boom. The boom ignores units (there are none in any ray now), so the camera can sit inside a unit.
2. The camera's liquid (`world.LiquidAt`) gives `underwater` (camera Z below the level).
3. Size: `cast` is a `thread_local Settings` copy (no allocation) with the cast width/height; a smaller frame is cast into a thread-local scratch and upscaled.
4. For each pixel (row-major, row 0 top): `PixelDirection`: yaw' = `((col+0.5)/W*FovH - FovH/2)`, pitch' = `FovV/2 - (row+0.5)/H*FovV`, equal-angle steps
   (not a pinhole projection); ray azimuth = view azimuth - yaw' (WoW yaw turns left), elevation = view elevation + pitch'. `CastStatic` = `Reach` then `Nearest`.
5. `Reach`: a 2D grid walk over 128-cell grids (533.333 yd) along the ray's horizontal path up to `REACH_MAX 4000`, ending at the first grid not created
   (`Tile().Loaded` false) or off the 64x64 map; that distance is the ray's limit (the "sky" boundary). A vertical ray (no horizontal motion) has reach 4000.
6. `Nearest` on `[0, limit]`: (a) static and dynamic tree casts (`SurfaceHit` with distance and the triangle's normal z facing the ray start); the nearer wins
   (`isDoor = doorHit && (!modelHit || door.Distance < model.Distance)`, so a tie is the static model); **every dynamic hit is `Class::Door`**; (b) WMO liquid if
   the ray descends (`dir.Z < 0`), cast only to the current best distance; (c) terrain via `CastTerrain`. Nearest wins; the earlier test wins a tie because
   comparisons are strict `<`.
7. `CastTerrain`: walks grid tiles, then single terrain cells (533.33/128 = 4.17 yd a side) inside each tile with the same `Walk` traversal; a tile is skipped when it has no heights/liquid
   or when the ray's lowest point over the tile is above the tile's `MaxHeight`; each solid cell is four triangles round the centre (corners from the map's V9, centre from V8,
   as `GridTerrainData::getHeight`) tested with one-sided Moller-Trumbore from above (`RayTriangleFromAbove`: a ray from under the terrain does not see its underside, and the
   normal is flipped to point up); a liquid plane is tested only when the ray descends and counts only if the ground at the crossing is not above the level. Liquid is read at the
   cell centre but treated as a plane over the whole cell.
8. `PaintHazards` (decision D5): a hit on terrain or a model with `NormalZ >= FLOOR_NORMAL (0.7)` inside a `HazardDisc` (`dx^2 + dy^2 <= r^2`, `|hit.z - disc.Z| <= HAZARD_REACH 0.5`)
   reads `Class::GroundHazard`. A flat decal: it hides nothing behind it (the ray ended where it did). `FrameHits` carries the painted class, as the pixel has it, so the map
   marks the hazard's area as an entity class (as the old disc cylinder did; the floor under it is not written from those rays). Cost: floor pixels x hazards (usually 0 to 3).
9. `ObjectiveFlag`: 1 when the segment from the camera to the hit (or to the reach for sky) passes within `radius` of the objective (default 1 yd; an object seen by the seek
    stage uses `ObjectiveRadiusFor(bound) = min(bound + 0.25, 1)`).
10. `EncodePixel` (4 bytes); `Upscale` if scaled. `hits`, when asked for, receives each cast pixel's `{Dir, Distance, Z, NormalZ, What}` at the cast size (not upscaled).
11. Returns the rays cast: `W*H` (if an image was asked for) plus 1 if there was a boom. `image == nullptr` casts no pixel at all (the scalars still fill).

`SegmentBlocked(world, from, to, ignore)` is the shadow ray: the static tree (any-hit, `VisionWorld::StaticAnyHit`), the dynamic tree (`DynamicAnyHit`, or with `ignore` set
the nearest `DynamicHit`, accepted when its `Object` is `ignore` and the rest of the segment is tested only as far as that surface), and the terrain from above
(`CastTerrain` over the segment, no liquids). No `Reach` walk, no liquid cast, no units. `MapVisionWorld` implements the any-hits with the ungated
`StaticVMapCollisionData::AnyHit` / `DynamicVMapCollisionData::AnyHit` (01-forge-core-delta.md).

Approximations and quirks worth knowing: (i) terrain triangles are one-sided; (ii) the boom ignores liquids; (iii) the liquid level of a cell
is sampled at its centre; (iv) a ray beyond 1000 yd saturates at byte 254, distinguishable from sky only by the class byte; (v) sky is "past the loaded grids", so which grids exist changes what the camera
shows (a ray over an uncreated grid reads sky though a player's client would draw terrain); (vi) a hazard is painted only where the floor is within 0.5 yd of the area's height.
No tests (removed 2026-10-07; see [tests.md](tests.md)); verify with the audit frames and evaluation videos, which draw the listed entities over the image.

## The entity sensor (EntitySensor.h/.cpp, entity-sensing)

Pure over `VisionWorld` and a candidate list. `GatherCandidates` (MapVisionWorld) visits the grid once and records only what placing an entity needs
(`SensorCandidate`: shape, GUID, feet, middle, radius, height, half-extents and axes for a box, the game object model for a door); `Sense` then:

1. collects every hazard candidate as a `HazardDisc` (all of them, seen or not: the frame paints them);
2. culls by the bounding sphere against the frame (`MayBeInFrame`: the centre's view angles within the field of view plus the sphere's angular radius, more in yaw near the poles);
3. ranks the survivors by squared distance from the camera (`rig.Camera`), ties by raw GUID: a total order;
4. nearest first, takes each candidate's sample points (`SamplePoints`), keeps those inside the frame (`ViewAngles`/`InFrame`: yaw' = wrap(`rig.Azimuth` - atan2(dy, dx)), pitch' = asin(dz/|d|) - `rig.Elevation`,
   |yaw'| <= FovH/2, |pitch'| <= FovV/2: the exact inverse of `PixelAngles`, so free look, zoom and the boom pull-in are all inside the `Rig`) and shoots a `SegmentBlocked` to each, ending `shorten`
   short of the sample (a body against a wall is not hidden by that wall); the entity is listed when at least one sample is inside and clear (D2), `los` = clear samples / samples tried; stops at `ENTITY_SLOTS` (32).

| shape | samples |
|---|---|
| unit | feet (`SAMPLE_LIFT 0.15` over the ground), middle, 0.95 of the height; plus +-0.8 radius across the view when the radius is over `BIG_RADIUS 1.5`; a corpse under `FLAT_HEIGHT 0.8`: the middle and +-radius across the view. Ends `min(radius, 0.5)` short. |
| model (a closed door) | centre, 0.8 of the half-height up, +-0.4 of the wide half-extent. Its own model does not block its rays (`ignore`). |
| box (no collision model) | centre, +-0.4 of the half-height, +-0.4 of the wide half-extent, along the object's rotated axes. Ends `min(wide half-extent, 0.5)` short. |
| hazard | the centre and four points of the rim at 0.9 radius (ahead, behind, across), on the ground (`SAMPLE_LIFT`). |

Entities behind other entities are listed (D6: a client draws nameplates through units). No minimum apparent size (D7): the list is geometric and resolution independent, range
`Settings::Range` (100 yd) plus the gather's zoom allowance. Cost: about 15 shadow rays typical, 5 x candidates in frustum worst case (a shadow ray is two tree traversals, now any-hit, and a short terrain
walk). `VisionBlock` reports them to `Cost::SensorRays`; `Breakdown::SegmentRays` counts them in a bench.

`ClassifySeen` runs `FactsOf`/`Classify` (quest and loot lookups) for the survivors alone, and `KillTargets` (the quest-log loop) at most once and only if a unit survived.

### Cost drivers per decision (per seat)

- `GatherCandidates`: one grid visit (creatures, players, game objects, dynamic objects) within `Range + zoom` of the seat plus the pivot offset; per candidate `CanSeeOrDetect` only.
  The expensive facts (`Classify`, quest and loot lookups, `KillTargets`) wait for the survivors.
- Sensor: sphere cull, then about 3 shadow rays a surviving candidate (5 for big units, hazards and boxes), nearest first, capped at 32 listed.
- Rays: about 2056 mean (see above); each is a `Reach` walk, two tree casts, a liquid cast when descending, a terrain cell walk to the nearest hit, and, on a floor hit, a test against each hazard disc.
  The per-ray unit and box loops that dominated (units x rays) are gone. `Breakdown` (`TreeNs/LiquidNs/TerrainNs/HazardNs`) measures it; the live total is `Vision::Cost` (VisionCost.h) shown in the status row.
- `Upscale`: O(pixels), 8192 for the canonical image.
- Map and memory costs: see cpp-vision-memory.md.

## Free look (FreeLook.h)

The look head chooses three categoricals each decision, in wire order: yaw rate (7 options: -180, -90, -30, 0, 30, 90, 180 deg/s, + left), pitch rate (5: -60, -20, 0, 20, 60 deg/s,
+ up), zoom (5: hold, in, out, recentre, face). `HEAD_SIZES {7,5,5}`, `NEUTRAL {3,2,0}`, zoom levels `{0, 3, 6, 12}` yd, pitch limit 80 deg. `State` (reset every episode by
`FreeLook::Reset` from `SeatState::ResetEpisode`, StageState.h:501): yaw offset, pitch, zoom level (nearest to the conf's zoom), held rates, `Observed`, `Render` size. `Apply` stores the held rates,
steps zoom in/out (clamped), recentre zeros yaw offset/pitch/rates (zoom kept), face returns the yaw offset as a body turn (0 when within 1e-3) which the caller passes to the controller as
`ControlState::FaceTurn` (a client SET_FACING, never a server SetFacing). `Advance(dt, bodyTurned)` subtracts the turn the controller really made from the offset, then integrates the held
rates over the decision (not at the first observation). The camera never swings back behind the facing ("never adjust camera"). Looking is free: a look choice never reaches `ApplySeatAction`
and has no mask (`Valid` rejects an out-of-range row, which then leaves the state alone).

## MapVisionWorld (MapVisionWorld.h/.cpp)

`VisionWorld` over a `Map`, composed of two halves (decision 0020). The static half -- `StaticHit`, `StaticAnyHit`, `ModelLiquid`, `Tile`, `Cell` -- is the map's `BakedWorld`, found in the
constructor by `SceneRegistry::Instance().Get(map->GetId())` (one immutable copy per map id, shared by every env and thread; `HasScene()` says whether the map has one; a map with none draws no static world and
logs once). The dynamic half -- `DynamicHit`, `DynamicAnyHit` -- is the live `DynamicMapTree` of the map (closed doors and other collision game objects, phase masked); `LiquidAt/FloorBelow` use an uncounted
`MapWorldQuery`. There is no fallback to the live static tree.

## Baked scenes (BakedScene.h, BakedWorld, SceneBaker, SceneBvh, SceneRegistry)

`BakedScene.h` is the scene file (format version 3: 512-byte header with the baker version, `SourceHash`, the terrain's height and liquid extremes and `SourceFilesDigest`, then 16-byte-aligned sections: triangles with normals and kinds, the BVH, WMO liquid triangles and BVH, the terrain
index, V9/V8 height floats, hole words, per-cell liquid level and kind, the per-block height and liquid ranges, the source model list). `BakedWorld` loads one file and answers the static half of `VisionWorld`. `SceneBaker::BakeMap(dataDir, mapId, outPath, ...)` makes it from the
extracted `vmaps/`, `maps/` and `dbc/LiquidType.dbc` of a server's DataDir (stock public collision APIs and a hand-read `.map` and `.dbc`; compiled with `-ffp-contract=off`, placement math in double, so the bytes are
machine-independent); `SourceIdentity` is the content hash it stores and `SourceDigest` the cheap pre-check (size and time of each source file; the content hash is only read when it differs, and refreshes the digest if the data is the same after all). `SceneRegistry::Ensure(dataDir, sceneDir, mapId, ...)` loads a scene if it is whole, of this baker's version and built from the data on disk, else bakes
it (temporary file, then rename) and loads it; `Get(mapId)` and `Checksums()` read the loaded set. The forge calls `Ensure` for every map of `StageMaps` at startup (`Forge::PrepareScenes`), into
`<AnimusForge.DataDir>/scenes`; the `scene_baker` tool is a command line over the same code (`bake`, `ensure`, `info`, `bench`, `verify`).

**The terrain cast with ranges (`CastTerrain`, `TerrainTile`, `TerrainExtent`, `CellFromTile`).** A world whose `Tile()` fills `Ranged` (the baked scene) lets the cast skip what cannot be hit; a world without it is cast cell by cell, exactly as before (that path is the reference of `scene_baker verify`). In order: (1) `Extent()` (the scene's lowest and highest ground height and liquid level, padded by `RANGE_PAD` = 0.1 yd) clips the ray to the heights where terrain exists, so a ray above everything and climbing, below everything and sinking, or level outside the band walks nothing; the walk then starts where the ray enters the band (only from a loaded origin grid, since the loaded grids are one rectangle). (2) Per grid: the old test (`lowest <= MaxHeight`), then the grid's `MinHeight`/`GroundMax` (and liquid `LiquidMin`/`LiquidMax`). (3) Super-blocks (4 x 4 per grid, 32 x 32 cells, derived at load from the blocks), then blocks (16 x 16 per grid, 8 x 8 cells, stored): the ray's heights at a square's entry and exit, padded, must overlap the square's [min, max]; the ground and the liquid are kept or dropped apart, so a block with no liquid never reads a liquid level. (4) Per cell: the ground's five heights bound its four triangles the same way, then the unchanged `RayTriangleFromAbove` and liquid plane test; a ranged tile's cells are read straight from its pointers by `CellFromTile` (which `BakedWorld::Cell` also calls). The culling only skips cells that no hit can be in, so hits are bit-identical to the cell-by-cell cast; an up-pointing ray is *not* skipped outright, because a steep terrain face can be hit from the front by a rising ray (the one-sided test is on the face's normal, not the ray's z).

`GatherCandidates(seat, pivot, range, GatheredSight&)` (map thread of the seat only, hence `AnimusForge.ObserveAfterJoin` is refused for vision stages): searches `range + |pivot - seat|` round the seat
(`Cell::VisitObjects`); keeps units within `range` of the pivot that `seat->CanSeeOrDetect` (the seat itself is not a candidate), hostile ground effects (`HostileGround`: an area spell's persistent area,
harmful, caster not friendly; a disc, listed as an object with Reaction -1 and `Radius`), and spawned game objects within range (by position) that the seat can see: with an enabled collision model as a
`Model` candidate (the model's bounds), otherwise a `Box` candidate from `GameObjectDisplayInfo` bounds (an open door too, whole: the doorway is clear to the shadow rays). `GatheredSight` holds the
candidates and the parallel `WorldObject*`s. `ClassifySeen(seat, gathered, sensed, SeenList&)` then builds each listed entity's `EntityInfo` (class via `FactsOf` + `Classify`, entry (0 for a player), level,
health share, reaction, centre, GUID, orientation, dead, open, used, plus `Radius`, `Height` and `Los` from the sensor). `FactsOf` uses core calls for reaction, loot rights, npc flags, quest status, lock skill,
`ActivateToQuest`, usable kinds; the core calls themselves are untested.

## SeenList / EntityInfo wire (entity list)

Byte format: the entity list is NOT a byte format. `SeenList` (Identity.h) is in-process: `Count`, camera, azimuth, elevation, seat level and `Info[32]` (`EntityInfo`; the pixel statistics `Stats[32]`
and the cast size are gone). `EntityInfo` fields in order: `Id{What,Quest,Lootable,Usable}`, `Entry u32`, `GameObject`, `Level`, `Health`, `Reaction i8`, `Centre Vec3`, `Guid u64`, `Orientation`, `Dead`, `Open`,
`Used`, `Radius`, `Height`, `Los`. It reaches the learner as FLOAT COLUMNS of the entities block: 32 slots x 20 features = 640 columns (`entities id=21 rev=2 obs=640`), feature order
(`EntitiesBlock.h`): present, class (raw index), type (raw entry), object, level/80, level_delta/10 clamped, health, reaction, quest, lootable, usable, distance (log-scaled as the pixel), yaw_sin, yaw_cos, pitch_sin, pitch_cos
(direction from the camera relative to the view), **los** (16: clear samples / tried), **ang_width** (17: `2 atan(radius / distance) / FovH`, clamped to 1), **ang_height** (18: `2 atan(height / 2 / distance) / FovV`,
clamped to 1), memory id (19). (Revision 1 had centroid_x, centroid_y, share at 16-18.) GUIDs never reach the observation (principle 1). Sight block
widths: 64 slots x 32 features + 23 named = 2071 (move3_interact); with the combat block 64 x 45 + 23 = 2903. These blocks are documented in cpp-blocks.md.

## The GPU camera (removed)

The GPU camera was removed 2026-10-08 (tag `archive/gpu-camera` holds the old state): `Gpu/Vision*`, `Device/Vision.hip`, `forge camera diff`,
`forge gpu scene`, `CastVision` in the device API. The CPU `Vision::Render` is the only camera. Revisit when shipping camera models to the realm.
What stays of `Gpu/` is the device-library loader and the device-buffer exchange with the learner (cpp-runtime-process-gpu.md).

## Accessors the forge added to upstream code (see 01-forge-core-delta.md)

Used by the camera (read-only, opt-in), the dynamic path only (the static world is baked; the static additions were deleted in stage 2): `DynamicVMapCollisionData::GetSurfaceHit(phase, ..., float& normalZ,
GameObjectModel const** model)` and `DynamicVMapCollisionData::AnyHit` (entity sensing: ungated any-hit segment tests, `DynamicMapTree::isInLineOfSight` with `ModelIgnoreFlags::Nothing`)
(`src/server/game/Maps/MapCollisionData.cpp`), `DynamicMapTree::GetIntersectionTime(..., G3D::Vector3* normal, GameObjectModel const** model)` and the `normal` out-parameter threaded through
`GameObjectModel::intersectRay` and `WorldModel/GroupModel::IntersectRay`. For movement: `ClientMovement::{Verify,Apply,Relocate}`,
`WorldSession::SanitizeMovementFlags`, `WorldSession::MovementOrders` and the `SendPacket` order filter.

## Tests

None: all forge tests were removed 2026-10-07 (tag `archive/with-tests`).

## Observed issues

1. (fixed with protocol 26: `Bridge/Protocol.h` said "4 bytes a pixel" while the code was five; it is four now.)
2. Camera.h, VisionCaster.h and FreeLook.h cite plan documents (`camera-vision.RAYCAST.md`, `.BYTES.md`, `.FREELOOK.md`, `.GPU.md` (GPU camera, removed), `.INTERFACE.md`) that are under `.agents/plans/` (gitignored) and not in the repository; they cannot be consulted.
3. `FreeLook.h` mentions "a scripted baseline" leaving rows neutral; scripted baselines no longer exist.
4. `Camera.h` `Resolution`/settings: a bad canonical size is only logged (ForgeConfig.cpp:299), the run continues until the learner refuses the manifest.
5. `Settings::Range` is a gather radius (`Range + zoom` plus the pivot offset): an entity just outside it is not listed though the line is clear, unlike a player's client.
6. (removed: the numbering and its grid-visit tie-break are gone; ties are by GUID.)
7. `VisionBlock::Observe` mutates the seat's `FreeLook::State` from a const block (comment says it is safe only because Observe runs once per seat per decision).
8. (removed with the GPU camera, tag `archive/gpu-camera`.)
9. Python `decode_map` derives "known" from the height byte while the C++ `MapBlock::Scalars` counts known as age != 255; a seen-free cell with no floor is known to the sim scalar and unknown to the learner (py-mappo.md).
10. Hazards are painted on the floor by the point-in-disc test, not drawn as shapes; the sensor lists them from their discs.
11. The sensor tests a few sample points: a thin occluder (bars, a grating, a tree M2) can show an entity through a gap a pixel ray would not find, or hide one a gap shows; the `los` column carries the confidence.
12. The final frame of an ended episode in an evaluation video takes its entity overlay from `FinalObs` with the layout `Layout[seat]` reports after the auto-reset (a layout change at a reset would read the wrong columns).

## Reviewer questions

- Is "sky beyond the loaded grids" acceptable for the shipped realm where the set of created grids differs from training instances?
- The pixel layout is fixed by the Python decode and the manifest: change them together or not at all (class table appends only).
- Is the sample-point sensor close enough to a client's nameplate rule for the realm (D2: listed with one clear sample)?
