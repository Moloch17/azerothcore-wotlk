# 0020: The camera casts against a baked scene file, not the live collision trees

**Date:** 2026-10-08 (stage 1; stage 2 pending)

**Decision.** The static world's pixels are traced against a flat scene file baked offline, one per map
(`<DataDir>/scenes/<map>.scene`), by a purpose-built tracer (`BakedWorld`), instead of through the core's nested
collision structures (`StaticMapTree` -> `ModelInstance` -> `WorldModel` -> `GroupModel`). The pixel format and the
manifest do not change; pixel values may shift against the old camera, and parity with it is not a goal.

**Reason.** The owner, 2026-10-08: the camera that helps the AI most and is fastest, the best solution rather than what
exists. A measured Stockades frame spent about 3.8 of 4.8 ms in the two tree casts per ray and about 0.5 ms in liquid
casts and 0.9 ms in a terrain walk that could not hit anything. A flat BVH over pre-transformed world-space triangles
needs no per-instance ray transform, no nested trees and no per-hit patch of the core, and its arrays upload as they are
to a GPU. The baker uses only stock upstream APIs, so the live realm can use the same files later.

**What it is.**

- `src/tools/scene_baker/`: reads `vmaps/` (`StaticMapTree::GetModelInstances`, `WorldModel::GetGroupModels`,
  `GroupModel::GetMeshData`, `WmoLiquid`) and `dbc/LiquidType.dbc`, writes world-space triangles with face normals, a
  kind byte (WMO or M2), a binned-SAH BVH, and WMO liquid as its own triangles and BVH tagged water/ocean/magma/slime and
  deadly. Deterministic; the file carries a checksum. The format (version 1, 512-byte header, flags that let a scene skip
  liquid and terrain passes) is in `.agents/plans/baked-camera/baked-camera.PLAN.md` section 1.2 and `BakedScene.h`.
- `BakedWorld` (`Runtime/Vision/`): the static half of `VisionWorld` (`StaticHit`, `StaticAnyHit`, `ModelLiquid`, `Tile`,
  `Cell`) over a loaded scene; `scene_baker bench`/`batch`/`verify` render with the real `Vision::Render` and check the
  tracer against a brute-force triangle test.
- Phase 1 keeps closed doors on the live dynamic tree and units as line-of-sight sensed. Phase 2 bakes a door-model
  library keyed by display id plus a runtime instance list from the public `GameObject` state; the tracer's instance
  primitive is designed for it (format slots reserved).
- Scene files change observations, so they belong in the cluster fingerprint and are shipped to every machine as one
  source with `maps`/`vmaps`. A map with no scene file refuses to start: no fallback and no flag (principle 17).
- Terrain (continents) is a documented later extension; v1 refuses a map that has `.map` tiles.

**Stage 1 result (Stockades, map 34, ten poses, 128 x 64, no units).** Same pictures: every pixel the same class; 99.8% the
same depth byte and 99.95% within one byte step (3.3% of the distance); of the 42 pixels in 81,920 that part by two or more
steps, 36 are cracks in the WMO mesh the baked tracer stops at and the old path passes through. About 3.2 times faster
(median frame 1.40 ms against 4.5 ms; the tracer alone about 0.9 ms of it, 4 times faster than the two tree casts).
The scene is 2.0 MB (29,190 triangles, 19,259 nodes), bakes in 0.02 s and loads in 1.8 ms. The tracer matches a brute-force
triangle test on 100,000 rays. Only a closed WMO was measured (no terrain, no liquid, no sky).

**Status.** Stage 1 (baker, format, tracer, bench, Stockades measured against the old camera) is done. Stage 2 (compose into
`MapVisionWorld`, delete the orphaned static Collision patch, fingerprint and distribution, the other instance maps)
remains. This supersedes the camera-core-patch plan. The measured result is in the stage 1 report.

See also [0007](0007-perceive-only-what-a-player-perceives.md), [principles.md](../principles.md) and the
[index](README.md).
