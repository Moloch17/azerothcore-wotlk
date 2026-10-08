# Vision: the camera (src/server/game/Animus/Vision/)

Reference for a manual review, read from `forge` bd32b9dc8. Paths are relative to `src/server/game/Animus/` unless they start
with `src/`. Line numbers are of that commit. Parts: this file (camera, caster, free look, classes, entity list),
[cpp-vision-memory.md](cpp-vision-memory.md) (mental map, entity memory), [cpp-vision-video.md](cpp-vision-video.md) (frame
images, evaluation videos). Related: [cpp-movement.md](cpp-movement.md), [cpp-blocks.md](cpp-blocks.md) (VisionBlock,
EntitiesBlock, MapBlock, SightBlock that consume these types), [cpp-runtime.md](cpp-runtime.md),
[01-forge-core-delta.md](01-forge-core-delta.md), [protocol.md](protocol.md), [py-mappo.md](py-mappo.md), [tests.md](tests.md),
[known-issues.md](known-issues.md).

## Purpose and scope

A seat sees through a ray-cast third-person camera (principle 1: only what a player sees). Each decision `VisionBlock::Observe`
(`Scenario/Curriculum/Blocks/VisionBlock.cpp:109`) advances the seat's free-look head, gathers the entities round the seat, casts a
frame with `Vision::Render`, and leaves: 11 float scalars (the block's columns), a byte image (5 bytes a pixel, 128x64 canonical) in the
seat's byte row, the frame's entity list (`SeenList`) for the entities and sight blocks, and the cast rays (`FrameHits`) for the map block.
The caster is pure over a `VisionWorld`; `MapVisionWorld` is the live-map implementation. The CPU caster is the only camera (see "The GPU camera (removed)").

## Map table (Vision/)

| path | lines | role |
|---|---|---|
| Vision/Camera.h | 426 | Settings, classes, pixel byte contract, scalars, constants, upscale, render-size draw; header-only except settings storage. |
| Vision/VisionCaster.h | 346 | `VisionWorld` interface, shapes (units, boxes, doors), `Sight`, `FrameSlots`, `Pose`, `CameraState`, `Rig`, `Hit`, caster API. |
| Vision/VisionCaster.cpp | 757 | Ray casting against trees, liquids, terrain cells, cylinders, boxes; pixel encode/decode; `Render`; `ParseRenderSizes`; `Configure/Current`. |
| Vision/FreeLook.h | 213 | The look head (yaw/pitch rate, zoom steps, recentre, face) and camera integration. |
| Vision/MapVisionWorld.h | 105 | `MapVisionWorld : VisionWorld` over a `Map`; `SightStore`; `GatherSight`, `FactsOf`, `KillTargets`, `HostileGround`. |
| Vision/MapVisionWorld.cpp | 461 | Implementations; the entity gather. |
| Vision/Identity.h | 138 | `EntityFacts`, `Identity`, `EntityInfo`, `SeenList`. |
| Vision/Identity.cpp | 84 | `Classify`: facts to semantic class and flags. |
| Vision/VisionCost.h | 67 | Process atomics for the vision status row (frame ns, rays, map ns, STEP bytes). |
| Vision/MentalMap.h, MentalMap.cpp | 307, 678 | Mental map: see cpp-vision-memory.md. |
| Vision/EntityMemory.h, EntityMemory.cpp | 130, 182 | Entity memory: see cpp-vision-memory.md. |
| Vision/FrameImage.h, FrameImage.cpp | 118, 426 | PNG/APNG writers, panels, composite: see cpp-vision-video.md. |
| Vision/EvalVideo.h, EvalVideo.cpp | 205, 494 | Evaluation video recorder: see cpp-vision-video.md. |

## The pixel: byte-exact (Camera.h, VisionCaster.cpp:645-666)

One pixel is 5 bytes (`BYTES_PER_PIXEL`, Camera.h:251). The image is `[row][col][byte]`, row 0 at the top, column 0 at the left;
canonical size 128 columns x 64 rows (`AnimusForge.Vision.Width/Height`), so `ImageBytes = 128 * 64 * 5 = 40960`. `EncodePixel`
(VisionCaster.cpp:645):

| byte | meaning | encoding |
|---|---|---|
| 0 | distance | `255` (SKY_BYTE) for sky, else `round(254 * clamp(ln(max(d, 0.25)/0.25) / ln(1000/0.25), 0, 1))`; so 0.25 yd is 0 and 1000 yd and beyond is 254; 255 is sky alone. |
| 1 | height of the hit over the seat's FEET | sky: 128; else `128 + clamp(round((hitZ - feetZ)/0.2), -125, 125)` (so +-25 yd range, 0.2 yd steps). |
| 2 | surface normal z | `round(255 * clamp(nz, 0, 1))`; sky and walls 0. |
| 3 | class and objective | bits 0-4 the class (`Class`, 0..23, CLASS_LIMIT 32), bit 5 (0x20) objective flag, bits 6-7 reserved (0). |
| 4 | entity slot | 0 none (or past the cap), else `s` in 1..32: the entity list's s-th entry. While a frame is being cast it holds the entity NUMBER (1..255) until slots are assigned. |

`DecodePixel` (:658) returns six floats (distance `b/254` or 1.0 for 255; height `(b-128)/125`; normal `b/255`; class `b & 31`; objective
`(b>>5)&1`; slot `b`). The learner's `decode_image` (`apps/forge/python/animus/mappo/networks.py:1110`) matches it exactly. No-frame rows
(director, absent agent, no map) are `{255,128,0,0,0}` per pixel (`FillNoFrame`, Camera.h:352), not zeros. A sky ray can still carry the objective
bit (`ObjectiveFlag` is tested to the ray's reach). Tests: `VisionTest.PixelsTravelAsFiveBytes` (VisionTest.cpp:718; every class, objective, slot,
distance/height/normal quantisation, all 256 byte values), `VisionTest.NoFrameRowIsSky` (:831). There is no test group named "CameraPixels".
Wire placement: the image travels in the STEP's image section (`Bridge/Protocol.h`, protocol 25); the block's float columns are the scalars alone;
live layout pins: `vision id=20 rev=5 obs=11` in every live stage (`src/test/.../LiveLayoutPin.golden.inc`, `LiveLayoutPinTest.cpp`).

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
every byte copied as is, so the slot and objective bit are copied). The expected cost is `(512 + 1152 + 2048 + 0.4*8192)/3.4 = 2056` rays a frame at the default
weights. The scalar `render_width` tells the network the size. Test: `VisionFreeLookTest.RenderSizeDraw*`, `UpscaleNearestPixel`, `ParseRenderSizes`;
`VisionTest.RenderAtADrawnSize`.

## Semantic classes (Camera.h:120)

A fixed table: values never change meaning, new classes take the next free value; the byte has room for 32. Count 24 now:
0 sky, 1 terrain, 2 model (static tree: WMO or M2), 3 door (dynamic-tree object or door/button), 4 water, 5 deadly (magma/slime), 6 hostile_creature,
7 neutral_creature, 8 friendly_creature, 9 hostile_player, 10 friendly_player, 11 quest_giver, 12 vendor, 13 trainer, 14 lootable_corpse, 15 corpse,
16 chest, 17 herb, 18 ore, 19 mailbox, 20 quest_object, 21 usable_object, 22 other_object, 23 ground_hazard (`Class::GroundHazard = 23`, Camera.h:148).
`CLASS_NAMES` (:158) is what the manifest prints. `Kind` (revision 4's 8 coarse kinds) follows from the class (`KindOf`, :178); `Deadly` and
`GroundHazard` both map to `Kind::Deadly`, every game-object class to `Door`. Rule: `Classify` (Identity.cpp:68) decides it from the seat's own client facts
(the UI rule: quest marks, lootable and quest objects are per character). First match wins: unit: dead -> lootable corpse (if this seat may loot) or corpse;
player -> hostile/friendly player; hostile -> hostile creature; quest mark -> quest giver; vendor; trainer; friendly/neutral creature. Game object: door or
button -> door; mailbox; quest mark -> quest giver; chest locked by herbalism -> herb, by mining -> ore; quest relevant -> quest object; chest; usable ->
usable object; else other. Tests: `VisionTest.ClassesFollowTheUiRule`.

## The caster (VisionCaster.cpp)

Contract (header comment VisionCaster.h:12-16): every pixel's ray is cast exactly against what the server has loaded; nothing is marched, baked, cached or skipped;
a ray ends at its first hit or is sky once it has left the loaded grids. `Render` (:668) in order:

1. `PlaceCamera` (:455): pivot = feet + `0.9 * BodyHeight` (PIVOT_SHARE); azimuth = pose yaw + camera yaw offset; elevation = camera pitch; zoom > 0 casts ONE ray from the
   pivot backwards (`Nearest` with an empty `Sight` and no liquids) and pulls the camera in to `back.Distance - 0.2` but never nearer than `min(0.3, zoom)`; camera =
   pivot - forward * boom. The boom ignores units, so the camera can sit inside a unit.
2. The camera's liquid (`world.LiquidAt`) gives `underwater` (camera Z below the level).
3. Size: `cast` is a `thread_local Settings` copy (no allocation) with the cast width/height; a smaller frame is cast into a thread-local scratch and upscaled.
4. For each pixel (row-major, row 0 top): `PixelDirection` (:477 area): yaw' = `((col+0.5)/W*FovH - FovH/2)`, pitch' = `FovV/2 - (row+0.5)/H*FovV`, equal-angle steps
   (not a pinhole projection); ray azimuth = view azimuth - yaw' (WoW yaw turns left), elevation = view elevation + pitch'. `CastRay` (:493) = `Reach` then `Nearest`.
5. `Reach` (:339): a 2D grid walk over 128-cell grids (533.333 yd) along the ray's horizontal path up to `REACH_MAX 4000`, ending at the first grid not created
   (`Tile().Loaded` false) or off the 64x64 map; that distance is the ray's limit (the "sky" boundary). A vertical ray (no horizontal motion) has reach 4000.
6. `Nearest` (:159) on `[0, limit]`: (a) static and dynamic tree casts (`SurfaceHit` with distance and the triangle's normal z facing the ray start); the nearer wins
   (dynamic wins a strict tie? no: `isDoor = doorHit && (!modelHit || door.Distance < model.Distance)`, so a tie is the static model); a dynamic hit takes the class and entity
   of the frame's `DoorShape` with the same `Model` pointer, else `Class::Door`; (b) WMO liquid if the ray descends (`dir.Z < 0`), cast only to the current best distance;
   (c) terrain via `CastTerrain`; (d) every non-self `UnitShape` cylinder via `RayCylinder` (all units tested by every ray: no culling); (e) every `BoxShape` via `RayBox`. Nearest wins; the
   earlier test wins a tie because comparisons are strict `<`.
7. `CastTerrain` (:356): walks grid tiles, then single terrain cells (533.33/128 = 4.17 yd a side) inside each tile with the same `Walk` traversal; a tile is skipped when it has no heights/liquid
   or when the ray's lowest point over the tile is above the tile's `MaxHeight`; each solid cell is four triangles round the centre (corners from the map's V9, centre from V8,
   as `GridTerrainData::getHeight`) tested with one-sided Moller-Trumbore from above (`RayTriangleFromAbove`, :57: a ray from under the terrain does not see its underside, and the
   normal is flipped to point up); a liquid plane is tested only when the ray descends and counts only if the ground at the crossing is not above the level. Liquid is read at the
   cell centre but treated as a plane over the whole cell (MapVisionWorld.cpp:107-112 vs :431-451).
8. `RayCylinder` (:503): side hit needs the origin outside (`c > 0`); caps are tested only from above for the top and below for the bottom; normal z 1 for a top hit, else 0.
   Hazards are discs (`HazardDisc`: radius of the area, thickness 0.2 yd, starting 0.05 below the area centre), drawn as `UnitShape` so every caster draws them the same way.
9. `RayBox` (:551): slab test in the object's rotated space; a ray starting inside sees none of the box; normal z from the entered axis row of the inverse rotation.
   Open doors are drawn as the top 15% of the closed box (`OpenDoorBox`, VisionCaster.h:293; `OPEN_DOOR_BAND 0.15`).
10. `ObjectiveFlag` (:636): 1 when the segment from the camera to the hit (or to the reach for sky) passes within `radius` of the objective (default 1 yd; an object seen by the seek
    stage uses `ObjectiveRadiusFor(bound) = min(bound + 0.25, 1)`).
11. `EncodePixel` with the entity NUMBER in byte 4; then `CountEntities` (per number: pixels, sum of rows, sum of columns; counts[0] holds no-entity pixels) and `AssignSlots` (numbers with a
    pixel, ascending (nearest first), up to 32); byte 4 becomes the slot (0 for a number past the cap, class kept). `FrameSlots` records cast size, the camera and the view angles.
    Then `Upscale` if scaled. `hits`, when asked for, receives each cast pixel's `{Dir, Distance, Z, NormalZ, What}` at the cast size (not upscaled).
12. Returns the rays cast: `W*H` (if an image was asked for) plus 1 if there was a boom. `image == nullptr` casts no pixel at all (the scalars still fill).

Approximations and quirks worth knowing: (i) no culling anywhere (cost is units x rays); (ii) terrain triangles are one-sided; (iii) the boom ignores units and liquids; (iv) the liquid level of a cell
is sampled at its centre; (v) a ray beyond 1000 yd saturates at byte 254, distinguishable from sky only by the class byte; (vi) units are upright cylinders and colliderless objects are display-bound boxes,
not their models; (vii) sky is "past the loaded grids", so which grids exist changes what the camera shows (a ray over an uncreated grid reads sky though a player's client would draw terrain).
Tests: `VisionTest.*` on fake worlds (:385-1547, e.g. `TerrainRaysHitTheTrianglesExactly`, `ThinRidgeTheMarchSteppedOver`, `LeavingTheLoadedGridsIsSky`, `DoorsAndModelsAreToldApart`,
`RaycastsAgainstTheOldMarch`, `ColliderlessObjectsAreCastAsBoxes`, `FramesNameTheirEntities`).

### Cost drivers per decision (per seat)

- `GatherSight`: one grid visit (creatures, players, game objects, dynamic objects) within `Range + zoom` of the seat plus the pivot offset; per candidate `CanSeeOrDetect` and
  `Classify` (quest and loot lookups; `KillTargets` loops the quest log every decision).
- Rays: about 2056 mean (see above); each is a `Reach` walk, two tree casts, a liquid cast when descending, a terrain cell walk to the nearest hit, then `units + boxes` tests.
  `VisionTest.TimingHarness` (:1049) and `Breakdown` (`TreeNs/LiquidNs/TerrainNs/UnitNs`) measure it; the live total is `Vision::Cost` (VisionCost.h) shown in the status row.
- `CountEntities/AssignSlots/Upscale`: O(pixels), 8192 for the canonical image.
- Map and memory costs: see cpp-vision-memory.md.

## Free look (FreeLook.h)

The look head chooses three categoricals each decision, in wire order: yaw rate (7 options: -180, -90, -30, 0, 30, 90, 180 deg/s, + left), pitch rate (5: -60, -20, 0, 20, 60 deg/s,
+ up), zoom (5: hold, in, out, recentre, face). `HEAD_SIZES {7,5,5}`, `NEUTRAL {3,2,0}`, zoom levels `{0, 3, 6, 12}` yd, pitch limit 80 deg. `State` (reset every episode by
`FreeLook::Reset` from `SeatState::ResetEpisode`, StageState.h:501): yaw offset, pitch, zoom level (nearest to the conf's zoom), held rates, `Observed`, `Render` size. `Apply` stores the held rates,
steps zoom in/out (clamped), recentre zeros yaw offset/pitch/rates (zoom kept), face returns the yaw offset as a body turn (0 when within 1e-3) which the caller passes to the controller as
`ControlState::FaceTurn` (a client SET_FACING, never a server SetFacing). `Advance(dt, bodyTurned)` subtracts the turn the controller really made from the offset, then integrates the held
rates over the decision (not at the first observation). The camera never swings back behind the facing ("never adjust camera"). Looking is free: a look choice never reaches `ApplySeatAction`
and has no mask (`Valid` rejects an out-of-range row, which then leaves the state alone). Tests: `VisionFreeLookTest.cpp` (14), `VisionProtocolTest.cpp` (look in SPEC/ACT).

## MapVisionWorld (MapVisionWorld.h/.cpp)

`VisionWorld` over a `Map` (`StaticHit/DynamicHit/ModelLiquid` call the forge-added tree accessors; `Tile/Cell` read created grids only (`IsGridCreated`, `GetCreatedGridTerrainData`
never create a grid); `LiquidAt/FloorBelow` use an uncounted `MapWorldQuery`). `GatherSight(seat, pivot, range, SightStore&)` (cpp:296; map thread of the seat only, hence
`AnimusForge.ObserveAfterJoin` is refused for vision stages): searches `range + |pivot - seat|` round the seat (`Cell::VisitObjects`); includes units within `range` of the pivot that
`seat->CanSeeOrDetect` (the seat itself as `Self`, never drawn), hostile ground effects (`HostileGround`: an area spell's persistent area, harmful, caster not friendly; drawn as a disc, listed as an
object with Reaction -1 and `Radius`), and spawned game objects within range (by position, not by model centre) that the seat can see: with an enabled collision model as a `DoorShape`, otherwise a
`BoxShape` from `GameObjectDisplayInfo` bounds (open doors as the top band). `EntityInfo` per candidate (class, entry (0 for a player), level, health share, reaction, centre, GUID, orientation, dead,
open, used). Numbering: squared distance from the pivot to each centre; `NumberNearest` stable-sorts (ties keep grid-visit order) and assigns 1..255, 0 beyond; `Entities[n]` holds number n's info.
`FactsOf` uses core calls for reaction, loot rights, npc flags, quest status, lock skill, `ActivateToQuest`, usable kinds. Tests: `VisionTest.EntitiesAreNumberedNearestFirst`, `SlotsAreTheSeenInOrderAndCapped`;
the core calls themselves are untested.

Reviewer notes: tie-break between equidistant entities follows the grid visit order, which is not specified (UNVERIFIED: whether it is deterministic across runs). A `GameObject`'s centre for numbering differs by
kind (model bounds centre, or box middle) while `within` uses its position.

## SeenList / EntityInfo wire (entity list)

Byte format: the entity list is NOT a byte format. `SeenList` (Identity.h:124) is in-process: `Count`, cast width/height, camera, azimuth, elevation, seat level, `Info[32]` (`EntityInfo`) and `Stats[32]` (`SlotStat`: entity number,
pixels, sum of rows, sum of cols). `EntityInfo` fields in order: `Id{What,Quest,Lootable,Usable}`, `Entry u32`, `GameObject`, `Level`, `Health`, `Reaction i8`, `Centre Vec3`, `Guid u64`, `Orientation`, `Dead`, `Open`,
`Used`, `Radius`. It reaches the learner as FLOAT COLUMNS of the entities block: 32 slots x 20 features = 640 columns (`LiveLayoutPin`: `entities id=21 rev=1 obs=640`), feature order
(`EntitiesBlock.h`): present, class (raw index), type (raw entry), object, level/80, level_delta/10 clamped, health, reaction, quest, lootable, usable, distance (log-scaled as the pixel), yaw_sin, yaw_cos, pitch_sin, pitch_cos
(direction from the camera relative to the view), centroid_x, centroid_y, share (`SlotCentroid`, Camera.h:229, integer sums), memory id. GUIDs never reach the observation (principle 1). Sight block
widths: 64 slots x 32 features + 23 named = 2071 (move3_interact); with the combat block 64 x 45 + 23 = 2903 (golden). These blocks are documented in cpp-blocks.md.

## The GPU camera (removed)

The GPU camera was removed 2026-10-08 (tag `archive/gpu-camera` holds the old state): `Gpu/Vision*`, `Device/Vision.hip`, `forge camera diff`,
`forge gpu scene`, `CastVision` in the device API. The CPU `Vision::Render` is the only camera. Revisit when shipping camera models to the realm.
What stays of `Gpu/` is the device-library loader and the device-buffer exchange with the learner (cpp-runtime-process-gpu.md).

## Accessors the forge added to upstream code (see 01-forge-core-delta.md)

Used by the camera (read-only, opt-in): `StaticVMapCollisionData::GetLiquidHit(x1,y1,z1,x2,y2,z2,float& distance, uint32& liquidType)` and `::GetSurfaceHit(..., float& distance, float& normalZ)`
(`src/server/game/Maps/MapCollisionData.cpp:117,148`); `DynamicVMapCollisionData::GetSurfaceHit(phase, ..., float& normalZ, GameObjectModel const** model)` (:230); `StaticMapTree::GetSurfaceIntersection/GetLiquidIntersection`
(`src/common/Collision/Maps/MapTree.cpp`), `DynamicMapTree::GetIntersectionTime(..., G3D::Vector3* normal, GameObjectModel const** model)`, the `normal` out-parameter threaded through `ModelInstance::intersectRay`,
`WorldModel/GroupModel::IntersectRay` and `WmoLiquid::IntersectRay`, `ModelInstance::intersectLiquid`, `GroupModel::IntersectLiquid`; `GridTerrainData::HasHeights/GetMaxHeight/GetCellHeights/HasLiquid/GetLiquidSurface`
(`src/server/game/Grids/GridTerrainData.h:262-271`, `.cpp:638-693`, plus `LoadedHeightData::gridMaxHeight`); `Map::GetCreatedGridTerrainData` (`Map.cpp:220`). For movement: `ClientMovement::{Verify,Apply,Relocate}`,
`WorldSession::SanitizeMovementFlags`, `WorldSession::MovementOrders` and the `SendPacket` order filter. `MMapData::ThreadQueryScope` (same file) is the runtime area's (cpp-runtime.md).

## Tests

VisionTest (1547 lines), VisionBlockTest, VisionEntitiesTest, VisionFreeLookTest, VisionProtocolTest, LiveLayoutPinTest, SightBlockTest (all under `src/test/server/game/Animus/`).

## Observed issues

1. `Bridge/Protocol.h:71-72` says the image has "4 bytes a pixel"; the code and the golden are 5 (revision 5). Comment/code disagreement.
2. Camera.h, VisionCaster.h and FreeLook.h cite plan documents (`camera-vision.RAYCAST.md`, `.BYTES.md`, `.FREELOOK.md`, `.GPU.md` (GPU camera, removed), `.INTERFACE.md`) that are under `.agents/plans/` (gitignored) and not in the repository; they cannot be consulted.
3. `FreeLook.h` mentions "a scripted baseline" leaving rows neutral; scripted baselines no longer exist.
4. `Camera.h` `Resolution`/settings: a bad canonical size is only logged (ForgeConfig.cpp:299), the run continues until the learner refuses the manifest.
5. `Settings::Range` is described in two ways ("the units a frame can see"; gather radius is `Range + zoom` plus the pivot offset); a unit just outside `Range` of the pivot but visible to the ray cast is simply not drawn (invisible), unlike a player's client.
6. `GatherSight` checks `within` by object position while numbering uses another centre; two near-equal distances may order by grid visit order.
7. `VisionBlock::Observe` mutates the seat's `FreeLook::State` from a const block (comment :124-128 says it is safe only because Observe runs once per seat per decision).
8. (removed with the GPU camera, tag `archive/gpu-camera`.)
9. Python `decode_map` derives "known" from the height byte (networks.py:1146) while the C++ `MapBlock::Scalars` counts known as age != 255; a seen-free cell with no floor is known to the sim scalar and unknown to the learner (py-mappo.md).
10. Hazard discs and open doors are drawn as units/boxes by design; their `EntityInfo::Radius` is only for hazards.

## Reviewer questions

- Is "sky beyond the loaded grids" acceptable for the shipped realm where the set of created grids differs from training instances?
- The pixel layout is pinned by `PixelsTravelAsFiveBytes`, the golden and the Python decode: change them together or not at all (class table appends only).
- Is the all-units-times-all-rays loop a problem when a crowd of 255 entities is gathered? No culling exists.
