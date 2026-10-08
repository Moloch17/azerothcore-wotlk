# 0020: The camera casts against a baked scene file, not the live collision trees

**Date:** 2026-10-08 (stage 1 and stage 2)

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

**Status.** Stages 1 and 2 are done. Phase 2 (doors baked as instances) and GPU residency remain.

See also [0007](0007-perceive-only-what-a-player-perceives.md), [principles.md](../principles.md) and the
[index](README.md).
