# Vision: frame images, composites, audit PNGs and evaluation videos (Vision/FrameImage.*, Vision/EvalVideo.*)

Read from `forge` bd32b9dc8; paths relative to `src/server/game/Animus/`. Part of the Vision reference: [cpp-vision.md](cpp-vision.md) (pixel
format, classes), [cpp-vision-memory.md](cpp-vision-memory.md) (the map crop drawn here). Output file formats are also summarised in
[file-formats.md](file-formats.md); the metrics side in [metrics.md](metrics.md); the operator side (`forgectl videos`, `collect-videos.sh`) in
[tools-and-ops.md](tools-and-ops.md).

## Map table

| path | lines | role |
|---|---|---|
| Vision/FrameImage.h | 118 | PNG panel/composite API, class colours, `RgbImage`, `ApngWriter`. |
| Vision/FrameImage.cpp | 426 | Decodes pixels as the learner does and draws them; own PNG/APNG chunk writer (zlib). |
| Vision/EvalVideo.h | 205 | Seed selection, filmed-seat selection, `EvalVideoRecorder`. |
| Vision/EvalVideo.cpp | 494 | Recorder with one worker thread; sidecars, index.json, index.html. |

## FrameImage

- `CLASS_COLOURS[24][3]` (FrameImage.cpp:29): one RGB per class, in class order (sky 30,30,80 ... ground_hazard 255,40,220).
- `ClassCounts(settings, image)`: pixels per class (class byte & 0x1F, clamped to 23).
- `FramePng(settings, image, scale, map=nullptr)` (:182): an RGB PNG of four panels side by side, each `W*scale` wide, `PANEL_GAP = 4` px of grey 64 between: depth (decoded distance as grey, sky white), class (class colour, objective pixels white), height (`(h+1)/2` grey, mid-grey at the feet), slope (normal z as grey). With `map` (the 6-byte crop) a fifth square panel as tall as the others: `MapPanel`.
- `MapColour(cell)`: unknown near black, floor green dimmed by age (half by 236 on the age byte), visited floor cyan, wall light grey, door orange, hazard blue, frontier yellow, an entity class colour over everything; `MapPanel(map, side)` draws the 48x48 crop nearest-cell with the body's four centre cells red.
- `CompositeRgb/CompositePng(settings, image, scale, map)` (:251/:246): one panel: the objective white; sky `COMPOSITE_SKY (30,30,80)`; else class colour x shade `(0.45 + 0.55 * normal)` x fog (`min(1, yards/80)^0.7` towards `COMPOSITE_HAZE (90,90,120) * 0.6`); a contour (x0.35) where a pixel and its right or lower neighbour straddle a whole yard of height over the feet (within the height channel's range only; levels are `floor((yards+100)/1)`). Yards are recovered from the distance byte via `NEAR * (1000/NEAR)^d`. With a map, a mini-map inset (side `max(8, outHeight/2)`, top right, grey frame). The slot byte is not drawn.
- `RgbPng` (:330) writes filter 0 and one IDAT (`compress2`, `Z_BEST_SPEED`), returns empty on failure; `WritePng` is internal.
- `ApngWriter(width, height, delayNum, delayDen)`: `Add(RgbImage)` filters each row (Up = 2 when equal to the row above, else Sub = 1), deflates it at once with `Z_BEST_SPEED, Z_RLE` and keeps only the packed bytes; `Finish()` writes signature, IHDR, `acTL` (frames, loop 0 = forever), per frame `fcTL` (offset 0, dispose none, blend source) then IDAT for the first and `fdAT` (sequence-numbered) for the rest, IEND. It is a valid PNG whose first frame is the still. Fails (returns false) on a size mismatch or deflate failure. Frames are whole (no blending).
- Callers: the training audit (`AnimusForge.cpp:1398 MaybeAuditCamera`, every `AnimusForge.Vision.AuditInterval` s of real time, `AuditSeats` seats of the deciding group, one of each layout first: writes `runs/<scenario>/camera/<stamp>-e<env>a<agent>-<layout>.png` (4 panels + map at `scale = max(1, 256/W)`) and `...-composite.png`, plus a CSV row with class, race, level, map, instance, position, facing, episode time, render size), `forge camera snapshot` (`cs_forge.cpp:544-555`), and the eval video worker.

Tests: `VisionFrameImageTest.cpp` (`FourPanelsAsTheLearnerDecodesThem`, `CompositeIsEveryLayerInOnePicture`, `TheMapPanelAndTheInset`), `VisionEvalVideoTest.AnimatedPngHoldsEveryFrame`, `CompositeFrameIsTheCompositePicture`.

## EvalVideo

### What gets filmed

Per evaluation (`AnimusForge.cpp:~1540-1590`, called when a MODE with `Mode == 1` (learner evaluation) starts): skipped when `AnimusForge.Vision.EvalVideos` is 0, the stage has no image, or an evaluation baseline is set. The seeds the sim plays are `[first, end)` over every data-parallel part;
`EvalVideoSeeds(first, end, count, pairs)` (EvalVideo.cpp:88) picks `count` of them deterministically (no randomness): `k`-th pick = pair `k*pairs/count` of cycle `k*cycles/count` with `cycles = max(1, episodes/pairs)`, offset `(cycle*pairs + pair) % episodes`, moved on to the next free seed on a collision, sorted. `pairs = StageScenario::EvaluationPairs()` (the (class, build) pairs: seed i plays pair `i mod pairs` and rung `i / pairs`), so the same seeds film the same episodes at every checkpoint and the picks spread over classes and rungs.
Label: the learner's env steps from `progress.json` when its phase is "evaluating", else `d<ticks>`; held-out arenas append `-heldout<n>`. Output directory `runs/<scenario>/videos/<label>/`.

Which seat: `CaptureEvalVideos(group)` (AnimusForge.cpp:1590), every decision after the env steps, per env of the deciding group: an env whose evaluating episode has a wanted seed and is not yet recording starts a recording. Candidates are the seats the learner plays (present agents; the "human" stand-in's and a director's seats are not present for the learner and are excluded). `EvalVideoAgent(pick, candidates)` (EvalVideo.cpp:124): candidates sorted by class then agent; if any has a role 1..3 (`FilmedRole`: 1 tank, 2 healer, 3 damage), the role is `(pick + step) % 3 + 1` (step 0..2, first non-empty), and of the seats in it the `(pick/3)`-th (mod count); otherwise `candidates[pick % n]`. The pick is the seed's place among the evaluation's seeds (`PickOf`).
The filmed seat's frame is `_pool->Image` for that agent (the camera image as sent to the learner, canonical size, after the CPU upscale) and, with a map block, its crop; at an episode's end the last frame comes from `FinalImage/FinalMapCrop` and the episode info row if the env's episode seed still equals the recorded one.

### Recorder (EvalVideo.h/.cpp)

`EvalVideoRecorder` (one per Forge): `Begin(options, seeds)` ends any previous evaluation first; `Wanted(seed)`; `PickOf`; `Start(episode)` (closes a leftover open recording as unfinished; removes the seed from the wanted list); `Frame(env, image, map)` copies `ImageBytes (+ MapBytes)` into a job (dropped and counted when queued bytes would exceed `MAX_QUEUED_BYTES = 512 MiB`; ignored past `MaxFrames`, then `Truncated`); `Finish(env, info)` queues a close job (the info row copied; null when the episode did not end); `End()` closes open recordings as unfinished and queues an index job; `Drain()` waits for the worker.
Threads: the calls are the world thread's; one lazily started worker thread (`Work`, :330) composites (`CompositeRgb` at `Scale`, default 4: 512x256 per frame, plus the inset), appends to the reel's `ApngWriter` (delay = `DecisionMs` / 1000 s, so the video plays at game speed), and writes the files. The destructor ends and joins.
Per video: `<seed>-<layout word>-<outcome>.png` (APNG) and a `.json` sidecar with: file, scenario, evaluation (label), seed, env, agent, layout, class, race, level, role (tank/healer/damage or ""), rung (the info column named `rung`/`tier`/`*_rung`/`*_tier`, not `at_top*`; null if none), outcome, frames, dropped_frames, bad_frames, truncated, decision_ms, fps, width, height, scale, camera ("WxH" canonical), render (the episode's cast size or null), map_inset, bytes, encode_ms_per_frame, and `measures` (every info column that is nonzero). Outcome (`EvalVideoOutcome`): the first column found among `found, arrived, won, cleared, success, survived` decides "success" (> 0.5) or "failure"; none found: "ended"; no row: "unfinished". At `End`, `index.json` (scenario, evaluation, selected_seeds, videos[]) and an `index.html` (images side by side, pixelated) are written, `LastDir` is set and "Eval videos: N recorded to <dir>" is logged.
`MaxFrames` = `2 * max(EpisodeSeconds, LongestEpisodeSeconds) * 1000 / DecisionMs + 16`.

Config keys: `AnimusForge.Vision.EvalVideos` (default 8, 0..64) and `AnimusForge.Vision.EvalVideoScale` (4, 1..8) (ForgeConfig.cpp:312-313); a different scale makes videos that do not compare (deploy-gate.md:457-458). The status row reports `EvalVideoCaptureUs`.

Tests: `VisionEvalVideoTest.cpp` (9): `SelectionIsTheSameForTheSameSeeds`, `SelectionSpreadsOverClassesAndRungs`, `SelectionKeepsToItsRange`, `OutcomeAndRungFromTheInfoRow`, `CompositeFrameIsTheCompositePicture`, `AnimatedPngHoldsEveryFrame`, `RecorderWritesVideosSidecarsAndIndex`, `NothingWithoutAnEvaluation`, `AFilmedSeatSpreadsOverThePartysPlacesAndClasses`.

## Observed issues

1. Two videos of the same seed and layout and outcome in one directory share a stem and the second overwrites the first (EvalVideo.cpp:374 stem `<seed>-<layout>-<outcome>`); possible when an env ends without its seed being seen again. UNVERIFIED how often this happens.
2. A reel with no frames logs a warning and writes nothing, but is not in the index (EvalVideo.cpp `WriteVideo` early return).
3. `Frame` dereferences `_evaluation->Options` after finding an open reel; correct only because `End` closes every reel first.
4. The audit's CSV and PNGs are written on the world thread (`MaybeAuditCamera`), unlike the videos' worker.
5. `FramePng`'s height panel and the composite decode with `(h+1)/2` and `HEIGHT_SCALE 25`: the sign convention differs between panels (documented in the header).
6. The eval-video selection assumes seeds are contiguous `[first, end)` across parts (`std::min/max` over parts) even when a part is empty or non-contiguous.
7. The APNG is built with a custom writer (`Chunk`, `Put32`) instead of a library; CRC via zlib `crc32`. No decoder test other than the writer's own round trip (UNVERIFIED: `AnimatedPngHoldsEveryFrame` parses chunks, not with a PNG library).

## Reviewer questions

- Should the filmed seat be fixed per seed across runs even when the party composition changes with the code (it is derived from the seed's party)?
- The worker can lag by up to 512 MiB of frames; a dropped frame is counted in the sidecar but a video with dropped frames still reads "success"/"failure".
