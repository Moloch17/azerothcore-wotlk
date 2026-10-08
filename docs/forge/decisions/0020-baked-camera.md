# 0020: The camera casts against a baked scene file, not the live collision trees

**Date:** 2026-10-08 (stages 1, 2 and 3)

**Decision.** The static world's pixels are traced against a flat scene file baked by the server that uses it, one per map
(`<AnimusForge.DataDir>/scenes/<map>.scene`), by a purpose-built tracer (`BakedWorld`), instead of through the core's nested
collision structures (`StaticMapTree` -> `ModelInstance` -> `WorldModel` -> `GroupModel`). The pixel format and the
manifest do not change; pixel values may shift against the old camera, and parity with it is not a goal.

**Reason.** The owner, 2026-10-08: the camera that helps the AI most and is fastest, the best solution rather than what
exists. A measured Stockades frame spent about 3.8 of 4.8 ms in the two tree casts per ray and about 0.5 ms in liquid
casts and 0.9 ms in a terrain walk that could not hit anything. A flat BVH over pre-transformed world-space triangles
needs no per-instance ray transform, no nested trees and no per-hit patch of the core, and its arrays upload as they are
to a GPU. The baker uses only stock upstream APIs, so the live realm can use the same files later.

**What it is.**

- `SceneBaker` / `SceneBvh` (`Runtime/Vision/`, with `src/tools/scene_baker/` a thin command line over them for `bake`,
  `ensure`, `info`, `bench` and `verify`): read `vmaps/` (`StaticMapTree::GetModelInstances`,
  `WorldModel::GetGroupModels`, `GroupModel::GetMeshData`, `WmoLiquid`), `maps/` (terrain) and `dbc/LiquidType.dbc`, write
  world-space triangles with face normals, a kind byte (WMO or M2), a binned-SAH BVH, WMO liquid as its own triangles
  and BVH tagged water/ocean/magma/slime and deadly, and the terrain tiles. Deterministic; the file carries a checksum.
  The format (version 2: version 1 plus the baker version, the source hash and the terrain sections; 512-byte header,
  flags that let a scene skip liquid and terrain passes) is in `BakedScene.h`.
- `BakedWorld` (`Runtime/Vision/`): the static half of `VisionWorld` (`StaticHit`, `StaticAnyHit`, `ModelLiquid`, `Tile`,
  `Cell`) over a loaded scene; `scene_baker bench`/`batch`/`verify` render with the real `Vision::Render` and check the
  tracer against a brute-force triangle test.
- Phase 1 keeps closed doors on the live dynamic tree and units as line-of-sight sensed. Phase 2 bakes a door-model
  library keyed by display id plus a runtime instance list from the public `GameObject` state; the tracer's instance
  primitive is designed for it (format slots reserved).
- **Scenes are baked by the server that uses them, at startup, and are never in git.** Distribution would have meant
  shipping 2 to 700 MB binaries that depend on the extraction; instead the baker is library code in `Runtime/Vision`
  (`SceneBaker`, `SceneBvh`, using only stock public collision APIs, with the `.map` terrain and `.dbc` files read by hand,
  so a realm's module on a stock core can run it) and the worldserver bakes every map a stage runs on when its file is
  missing or stale (`SceneRegistry::Ensure`, `Forge::PrepareScenes`, before the sim starts). A scene is stale when its
  magic, version, checksum or recorded baker version is wrong or the content hash of the source it was built from
  (vmtree, vmtiles, the models its spawns name, the `.map` tiles, `LiquidType.dbc`) differs from the data on disk now;
  content, not size or time, because docker and rsync change times. A bake writes a temporary file and renames it, so two
  processes starting together leave one whole file. A map with no scene that cannot be baked refuses to start (`forge
  start` names the map, the path and the cause): no fallback and no flag (principle 17).
- **Where they live.** `<AnimusForge.DataDir>/scenes/<map id padded to 3>.scene`. `AnimusForge.DataDir` is the one Animus
  data directory (the scenes today; the models of a realm already sit beside them); its default is `modules/animus` in
  the directory the server loaded `worldserver.conf` from (the same default a stock realm's module sees, and the folder
  the module's cmake, `ANIMUS_MODELS_INSTALL_DIR`, installs models into). The directory must be writable and persisted:
  under Docker only `bin/` and `etc/` reach the runtime image, `etc/` is the writable volume, and the extracted game data
  (where the scenes were first going to be written) is mounted read-only. CMake creates the empty folder at install
  beside the config copy; `*.scene` is in `.gitignore`. The key is per machine and is not compared by `forgectl`. The
  module follow-up should expose the same setting (`Animus.DataDir`) and pass `<DataDir>/scenes` to the shared API.
- **Cluster.** Each machine bakes its own, so the cluster fingerprint gains `scenes=<map>:<checksum>,...` (each loaded
  scene's header checksum, ascending by map id) and the host refuses a worker whose scenes differ. The bake is
  deterministic across machines: placement math in double, rounded to float once, `-ffp-contract=off` on the baker's
  translation units, total-order sorts, no threads; bytes were identical between -O0, -O3 and -O3 -march=native builds and
  across cores. Residual risk: the C library's `sin` and `cos` of a few angles per spawn (double); a different libm could
  in principle differ in the last bit of a rotation and so the checksum. `forgectl doctor` (`camera scenes`) shows which
  machine differs; copy the host's file over the worker's, or delete it and restart to bake again.
- Terrain (format 2): the baker reads each `.map` tile (heights as float, uint16 or uint8, holes, liquid) and stores the
  V9/V8 floats, hole words and a per-cell liquid level and kind, so `Tile` and `Cell` answer as `GridTerrainData` does
  (the liquid area override of `AreaTable.dbc` is not applied). A continent bakes in 4-5 s into 650-710 MB.

**Stage 1 result (Stockades, map 34, ten poses, 128 x 64, no units).** Same pictures: every pixel the same class; 99.8% the
same depth byte and 99.95% within one byte step (3.3% of the distance); of the 42 pixels in 81,920 that part by two or more
steps, 36 are cracks in the WMO mesh the baked tracer stops at and the old path passes through. About 3.2 times faster
(median frame 1.40 ms against 4.5 ms; the tracer alone about 0.9 ms of it, 4 times faster than the two tree casts).
The scene is 2.0 MB (29,190 triangles, 19,259 nodes), bakes in 0.02 s and loads in 1.8 ms. The tracer matches a brute-force
triangle test on 100,000 rays. Only a closed WMO was measured (no terrain, no liquid, no sky).

**Stage 2 result.** `MapVisionWorld` takes its static hits, any-hit, WMO liquid, tile and cell data from the map's
`BakedWorld` (one immutable copy per map id, shared by every env and thread) and keeps the live dynamic tree for doors; the
camera's patch of the static collision path in `src/common/Collision`, `MapCollisionData`, `GridTerrainData` and `Map` is
deleted (see `reference/01-forge-core-delta.md` section G for the before and after). The maps of the ten live stages are
34, 36, 389 and Wailing Caverns 43 (a held-out evaluation arena); all four bake in under 0.05 s. Deadmines (36) has
36 terrain tiles, of which 24 have heights, and the baked camera matches the old one on its terrain and WMO water. See the
stage 2 report for the per-map numbers and images.

**Stage 3: the terrain cast, made fast and kept exact (format version 3).** Stage 2 found that on a map with terrain
the terrain cast dominated: Deadmines (36 terrain tiles, 24 with heights) spent about 1.1 to 1.4 ms of a 2.05 to 2.6 ms
frame in it even inside the mine, and an outdoor view cost 44 to 50 ms, because a ray that never meets ground walked
every cell of every height-bearing grid along its path. The cast now culls with ranges stored in the scene and gives
the same hits as the cell-by-cell walk.

- *What is stored (format 3, `BakedScene.h`).* Each height-bearing terrain record gets its `MinHeight` (the reserved
  slot; `MaxHeight` keeps the map file header's value, so the grid test of the old walk is unchanged). Two new sections
  hold, per height-bearing non-flat tile and per liquid tile, 16 x 16 `BlockRange {Min, Max}`: the lowest and highest
  height (corners of the 9 x 9 vertices and centres of the 8 x 8 cells, holes included) or liquid level (cells with
  liquid only; an empty block reads FLT_MAX / -FLT_MAX) of each 8 x 8 block of cells. The header carries the scene-wide
  extremes of ground and liquid. The reader derives one more level, 4 x 4 super-blocks of 4 x 4 blocks, at load. A scene
  of version 2 fails the version check and is baked again at the next start through `SceneRegistry::Ensure` (tested
  with `scene_baker ensure`).
- *The culling rules (`CastTerrain`).* (1) The ray is clipped to the heights where terrain exists, the scene's ground
  band (and the liquid band when liquids are cast and the ray descends), padded by 0.1 yard: nothing is walked for a
  ray above everything and climbing, below everything and sinking, or level outside the band, and a descending ray
  starts walking where it enters the band (only from a loaded origin grid: the loaded grids are one rectangle, so
  the path stays inside until it leaves). (2) A grid is walked if the old test passes and the ray's heights over the
  grid's footprint overlap the grid's [MinHeight, GroundMax] (ground) or its liquid band. (3) Then super-blocks and
  blocks, each by the ray's heights at the square's entry and exit against the square's range, the ground and the liquid
  apart. (4) Then a cell's five heights against the ray's heights over the cell, before the unchanged four-triangle
  test (V8/V9 rule, holes, liquid planes). The comparison is on a padded range, so rounding cannot drop a cell a hit
  is in. **A rising ray is not skipped outright** (the stage 3 brief said rays pointing up never walk): the terrain's
  triangles are one-sided by their normal, and a steep face can be hit from the front by a rising ray, so a ray below the
  highest ground is walked whichever way it points; a rising ray above the highest ground, which is what the brief
  meant, ends at once.
- *Exactness.* `scene_baker verify` casts random terrain rays and the poses' pixel rays through the whole caster with
  the ranges and again with them hidden (the old walk) and requires every field of the hit to match bit for bit: 3,000,000
  random rays on map 36 (219,493 that hit ground or liquid, 17,047 liquid) and 73,728 pixel rays: 0 differences. The one
  theoretical gap is a ray through the exact shared corner of four cells, where the nested walk (reciprocal multiply,
  restarted at each block) and the reference (divide, one pass) could pick a different first cell; none occurred.
- *Source identity, cheaper.* The startup check used to read and hash every vmtile, model and `.map` file of a map at
  every start. The scene now stores `SourceFilesDigest`, FNV-1a 64 over the (name, size, mtime in ns) of every source
  file (the model list is stored too, so it can be rebuilt without parsing the tree). The rule in `SceneRegistry::Ensure`:
  if the digest of the files now equals the stored one the scene is current and no file is read; otherwise the content
  hash (`SourceIdentity`) decides exactly as before, a differing hash bakes again, and an equal one rewrites the 8 digest
  bytes in place and keeps the scene. The content hash stays authoritative; the digest only skips it, and is wrong
  only if a source file changes without changing its size or its nanosecond mtime (a deliberate `touch -r`). The digest
  is machine-specific, so it is outside the checksum: the cluster still compares equal checksums. (A latent quirk found on
  the way: the spawn name of an M2 ends in a NUL, so the old hash read `Name.m2` for those, not `Name.m2.vmo`;
  the digest and the hash both still cover exactly the files the operating system opened.)
- *Results* (`var/s3/bench_table.md`; pinned core, 300 reps; old-baked = the stage 2 code on the same machine). Deadmines
  interior poses 2.10 to 2.70 ms to 1.74 to 2.16 ms (terrain from about 1.1 to about 0.4 ms, the static trace alone is 0.9 to
  1.4 ms); open-air poses with water 46 to 54 ms to 3.0 to 3.7 ms (12 to 15 times); maps 34 and 389, which have no terrain, 4 to
  7 percent faster. The targets (interior 1.6 ms, open air 3 ms) were not met: interior frames are bound by the static
  BVH trace and the liquid cast (1.2 ms of the frame before any terrain), and the open-air frames by about 16 cells and
  as many blocks a ray that all lie within the ground's relief band, at about 20 ns each.
- *Liquid against the old camera.* On the four open-air water poses and the four lava poses every liquid pixel the
  old camera has is the same class at the same depth byte in the baked one (2,065 of 2,066 water pixels; 24,059 of
  24,060 deadly pixels, one lava pixel is a WMO model in the baked one); the baked camera draws 338 more water pixels
  (14 percent), all in the far field, where the live server had not loaded the grid (the old camera's rays stop at
  the loaded grids; the baked scene holds every `.map` tile of the extraction).

**Status.** Stages 1, 2 and 3 are done. Phase 2 (doors baked as instances) and GPU residency remain.

See also [0007](0007-perceive-only-what-a-player-perceives.md), [principles.md](../principles.md) and the
[index](README.md).
