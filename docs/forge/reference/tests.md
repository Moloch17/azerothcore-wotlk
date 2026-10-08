# Tests

Purpose and scope. The map of every test in the forge: the GTests under `src/test/server/game/Animus` (the only
GTests the forge adds; no other test directory mentions Animus or Forge), and the pytests under
`apps/forge/python/tests`. For each: what it covers, what it needs (torch, a GPU, the C++ sources, a configured build,
machine data), which are slow, which golden and fixture files pin behaviour and how to regenerate them, how to run each
suite, and what none of them covers. Related: [tools-and-ops.md](tools-and-ops.md) (`forgectl test`),
[config-yaml.md](config-yaml.md), [stages.md](stages.md), [known-issues.md](known-issues.md),
[py-learner.md](py-learner.md), [protocol.md](protocol.md).

## Map of the files in this area

| Path | Lines | Role |
|---|---|---|
| `src/test/CMakeLists.txt` | 59 | collects every `.cpp` under `src/test` into the `unit_tests` target and registers `add_test(unit)` |
| `src/test/server/game/Animus/*Test.cpp` | 14,385 in all (see section 1) | 45 forge GTest files |
| `src/test/server/game/Animus/LiveLayoutPin.golden.inc` | 149 | the pinned layout of every live stage |
| `apps/forge/python/pyproject.toml` | 29 | pytest configuration (`testpaths`, `addopts = -m 'not slow'`, `slow` marker) |
| `apps/forge/python/tests/conftest.py` | 8 | docstring only (how to run); defines no fixtures |
| `apps/forge/python/tests/sim_threads.py` | 27 | helper: fake-sim threads with socket timeouts, daemon threads, failing joins |
| `apps/forge/python/tests/slow_gae_reference.py` | 79 | helper: the per-agent GAE loop that `compute_span_gae` is checked against |
| `apps/forge/python/tests/human_capture_writer.py` | 243 | helper: test-only writer of capture records written from the format document |
| `apps/forge/python/tests/test_*.py` | 17,000 in all (see section 2) | 88 test files |
| `apps/forge/python/tests/fixtures/` | 6 files | `recorded_m2_episode_info.json`, `seek_spec.json`, `seek_stage.json`, `stage_move1_controls.json`, `stage_move2_seek.json`, `test_stage.yaml` |
| `apps/forge/python/tests/golden/` | 7 files | `learner_update.json`, `seat_attention.{amdl,json}`, `seat_attention_recurrent.amdl`, `seat_sets.{amdl,json}`, `seat_sets_recurrent.amdl` |
| `apps/forge/python/tests/update_stats_reference.json` | 28 | the recurrent update's reported statistics |

## How to run each suite

- **Everything the tool supports, in the dev container: `forgectl test`** (`forgectl test --gpu` runs the pytest on the
  card).
  It runs `apps/forge/tools/forgectl-test.sh` inside the container named in `cluster.toml` (`claude-syntax`):
  1. configure the tree if `$BUILD/CMakeCache.txt` is missing (clang, `RelWithDebInfo`, `BUILD_TESTING=ON`,
     `MODULES=static`, `SCRIPTS=static`, `APPS_BUILD=all`, `TOOLS_BUILD=none`, no PCH, compile-commands export:
     `forgectl-test.sh:33-37`), else re-configure (line 39);
  2. `cmake --build $BUILD --target unit_tests -j<jobs>` under `nice -n 15`. The script treats a failed link as expected
     and counts only compiler `error:` lines as a failed build (lines 42-50);
  3. **the llvm-17 relink** (lines 52-60): the container's clang is version 18 and its resource directory has no
     compiler-rt libraries (`libclang_rt.profile`, needed by the instrumented link); llvm-17's has them. The script
     reads
     `src/test/CMakeFiles/unit_tests.dir/link.txt`, substitutes `/usr/bin/clang++` with `/usr/bin/clang++
     -resource-dir=/usr/lib/llvm-17/lib/clang/17` and runs the link line itself. If the compiler path in `link.txt` is
     not exactly `/usr/bin/clang++` the substitution does nothing and the link fails ("FATAL link failed");
  4. runs `./unit_tests` and greps its summary lines;
  5. runs `$PY -m pytest -q tests/ -p no:cacheprovider` in `apps/forge/python`, with `HIP_VISIBLE_DEVICES=""` exported
     for the CPU run and unset with `--gpu` (lines 71-74). **The `HIP_VISIBLE_DEVICES` convention:** an empty value
     hides
     the AMD (ROCm) GPU from torch, so CPU-only runs are the default even in a container that has the card; the deploy
     gate uses `HIP_VISIBLE_DEVICES=0` for the GPU run on the dev machine (deploy-gate.md step 3). On a CUDA machine the
     variable has no effect (UNVERIFIED: the script sets only the HIP one).
  The first build of a tree takes tens of minutes. Build directory: `<mount>/var/forgectl-build-<tree name>` unless
  `--build-dir`. The script's output lines (`UNIT_EXIT`, `UNIT_LINE`, `UNIT_FAILED`, `PYTEST_EXIT`, `PYTEST_LINE`,
  `PYTEST_FAILED`, `FATAL`) are parsed by `forgectl/testcmd.py`.
- **GTests alone:** build target `unit_tests`; run `./unit_tests --gtest_filter='<pattern>'`. Several tests skip unless
  environment variables point at data (section 1).
- **pytest alone**, in the dev container with the learner venv (the host's python has no torch, `conftest.py`):
  `cd apps/forge/python && .venv/bin/python -m pytest -q` (fast suite), `-m slow` (only the slow ones), `-m ""`
  (everything). The venv is created by `apps/docker/animus-venv.sh` on container start.
- Many pytests read the C++ sources and docs through `Path(__file__).parents[4]`, so they need the whole checkout, not
  only
  `apps/forge/python`.
- `forgectl`'s own tests (`test_forgectl.py`) replace ssh, docker and subprocess with fakes, so they touch nothing.

## 1. GTests

All are in `src/test/server/game/Animus/`, in the single `unit_tests` binary (the file list is collected by glob, so a
new file needs a CMake re-configure). They link the whole `game` library. Counts are `TEST*` macros; a macro can be a
fixture family.

Skips: tests that need data skip with `GTEST_SKIP` when an environment variable is unset: `FORGE_VISION_DATA` (the data
directory with `maps/` and `vmaps/`: `DeadminesSitesDataTest`, `StockadeRoomsDataTest`, `StockadeHallwaysDataTest`,
`VisionGpuDataTest`), `FORGE_STOCKADE_SCAN`, `FORGE_DEADMINES_SCAN`, `FORGE_SIGHT_AUTHOR` (the "authoring" scans that
regenerate site tables), `FORGE_GPU_RUNTIME`/`FORGE_GPU_LIBRARY` or `FORGE_VISION_EMULATE` (`VisionGpuDataTest`).
`forgectl-test.sh` sets none of them, so those tests are skipped in the standard run (UNVERIFIED: the exact skipped
count; the example in forgectl.md shows 2). `CompassBlockTest.cpp:77` skips with "move2_seek is not defined yet", a
leftover that can no longer trigger (move2_seek exists).

| File | Lines | Tests | What it covers (live behaviour it pins) |
|---|---|---|---|
| `BotAccountsTest.cpp` | 38 | 1 | the account-id range seats start from |
| `BuildRetryTest.cpp` | 46 | 2 | a failed episode build is drawn again; a stage that keeps failing is given up on |
| `ClientTest.cpp` | 773 | 14+ | the emulated client: body = truth, server has the last report, spawn under/inside ground, cadence independent of tick, root, mouse-turn facing reports every 0.1 rad, north wrap, refused report puts the body back |
| `CombatPerceptionTest.cpp` | 822 | 13+ | combat stages: nothing behind walls is listed, enemies = frames' living hostiles, own frames always known, threat status as the client shows it, combat columns on sight slots, rung pulls and outcome scaling, corridor points, respawn clock and rejoin |
| `CompassBlockTest.cpp` | 81 | 3 | compass block columns = the move block's old objective columns; M1 has it, seek does not |
| `DeadminesSitesDataTest.cpp` | 253 | 3 | Deadmines authored sites are on clear floor (needs `FORGE_VISION_DATA`) |
| `DungeonStagesTest.cpp` | 627 | 13+ | G2/D1-D3: layouts and encounters, Wailing Caverns never drawn in training, packs cleared in route order, chain pulls, wing tier = ladder rung, purposes are outcomes and prices costs, stand-in share in every party stage, every boss measured, Deadmines doors/levers/cannon through handlers, movement stages unchanged, lost priced from the actual leader |
| `EntityMemoryTest.cpp` | 239 | 7 | entity memory: only what the frame shows is written, a kill out of sight stays a place until the corpse is seen, ageing, stable ids, oldest forgotten first, patrol course, recall |
| `FloorScanTest.cpp` | 60 | 2 | floor-scan cell classification and grid spans |
| `GoalObjectiveLeakTest.cpp` | 94 | 2 | a withheld compass leaves the goal travel unreached; a stage without a compass never learns the place (principle 2) |
| `InteractStageTest.cpp` | 694 | 12 | M3: layout, sites, ladder, decoys and evaluation draws, wrong-object press priced, door paid once, named row always in the sight block, lever press opens its door, key item opens lock, open-door band |
| `KinematicsTest.cpp` | 71 | 3 | kinematics sample columns, mode order, clamps |
| `LiveLayoutPinTest.cpp` | 219 | 1 | **every live stage's block list, ids, revisions, widths, column and action names** against `LiveLayoutPin.golden.inc` (see below) |
| `MapWorldQueryTest.cpp` | 53 | 2 | body sweep rays and share at a wall |
| `MentalMapTest.cpp` | 492 | 11 | the mental map: writes come from cast pixels and body, voids, height layers, crop turns with facing, two-yard cell, frontier, persistence and ageing, caps and coarse tiles, crop byte round trip, map block description |
| `MoveBlockTest.cpp` | 82 | 2 | the move block has no ray columns, columns named, none is the objective |
| `MoveControlsTest.cpp` | 144 | 4 | controller layout at revision 2, only the impossible masked (principle 5), presses set held controls, reversals priced by recency |
| `MovePriceTest.cpp` | 89 | 6 | movement prices: recency decay, reversals/weaves, angle undone, bearing swing, steering effort, charged once settled |
| `PartyFollowTest.cpp` | 503 | 15 | M4: fake death rises after delay and walks back, rejoin measure, follow spacing, blocking, regroup, ladder stops, terms are outcome/cost, party frames block (minimap dots, revision 2 layout), M1/M2 layouts unchanged |
| `PlayerControllerTest.cpp` | 939 | 12+ | the player controller: ground speeds, turning rates, steps vs walls, slopes to 50 degrees, falls timed by gravity, jump apex, wall slide, swimming, swim jumps |
| `ReplayTest.cpp` | 227 | 3 | capture round trip, replay with no drift, a displaced recording drifts |
| `ReportCadenceTest.cpp` | 53 | 2 | a change sends its opcode at once; heartbeat half a second after the last packet while moving |
| `ResetSamplesTest.cpp` | 163 | 6 | reset-time quantiles, rolling window, concurrent adds, stall naming |
| `RewardLedgerTest.cpp` | 148 | 5 | score = outcome + cost before tier/role; shaping and cost scales touch only their category; every term has a name and category |
| `RolesStageTest.cpp` | 401 | 10 | G1: stage defined, packs by rung, pull drills camp, drill terms are outcomes/costs, hold/keep/focus/pull drills, death rises at the entrance, wipe rises together |
| `RouteShortcutTest.cpp` | 93 | 4 | route walking in legs, corners, winding routes, a closed door ends the run |
| `SeekEncounterTest.cpp` | 338 | 10 | M2: stage defined, room table, object pool, ladder placement by rung, evaluation plays the training rung, episode length, objects uniform and every room covered, aids are shaping and finding is the outcome |
| `SeekFlagTest.cpp` | 178 | 4 | the objective flag: radius, visible/hidden by crate or wall |
| `SeekTest.cpp` | 91 | 2 | a scripted test actor turns, runs and arrives; nothing held on arrival (a test fixture, not a policy) |
| `SightBlockTest.cpp` | 482 | 10 | the sight block: list and pointers, no press loots, object use judged as the handler does, cast packet and refusal, visible-then-remembered, masks only empty slots, select through the handler, out-of-reach presses refused and priced, movement stages unchanged |
| `SightEncounterTest.cpp` | 423 | 10 | M1: hallway table, evaluation pairs fixed, compass withheld more each rung, in-sight = one camera ray from the eye, placement, wall/stuck on the cost ladder, stop measured from the object's side |
| `StandingTest.cpp` | 79 | 4 | wall charges only uncovered ground, stopped reads the server's flags, follow bands, course kinks |
| `StandInTest.cpp` | 132 | 6 | the stand-in partner: style draws deterministic per seed, styles cover every choice, role within build, row present, none without the mode flag |
| `StockadeHallwaysDataTest.cpp` | 306 | 3 | Stockade hallway points on clear floor, evaluation pairs (needs `FORGE_VISION_DATA`) |
| `StockadeRoomsDataTest.cpp` | 155 | 2 | Stockade room samples on the floor (needs `FORGE_VISION_DATA`) |
| `VisionBlockTest.cpp` | 120 | 2 | the vision block: scalars alone plus image bytes, canonical defaults |
| `VisionEntitiesTest.cpp` | 159 | 3 | the entity list is a set of the visible; a camera brings its list; slots carry the plan's fields |
| `VisionEvalVideoTest.cpp` | 541 | 9 | evaluation videos: same selection per seed, spread over classes and rungs, outcome/rung from the info row, composite frame, animated PNG, recorder writes videos/sidecars/index |
| `VisionFrameImageTest.cpp` | 269 | 3 | the four-panel frame as the learner decodes it, composite, map panel and inset |
| `VisionFreeLookTest.cpp` | 498 | 14 | free look: reset, held rate, yaw wrap, pitch clamp, zoom steps, recentre, choices in range, nearest-pixel upscale, render-size draw and weights, `ParseRenderSizes`, face turns the body |
| `VisionGpuDataTest.cpp` | 537 | 7 | GPU/emulated renderer against real map data (Stockades, Barrens, lake, forest, Elwynn, crowd, door); needs data and a device or `FORGE_VISION_EMULATE` |
| `VisionGpuTest.cpp` | 633 | 10 | the device renderer's packing and traversal (model instances, liquids, BIH walk, terrain cells, request frames, library version check) on the CPU |
| `VisionProtocolTest.cpp` | 173 | 5 | SPEC look heads, ACT look, protocol-21 ACT without vision, out-of-range look refused |
| `VisionTest.cpp` | 1547 | 18+ | the camera and ray caster: pixel rays, boom, terrain/liquid/model hits, sky rules, doors vs models, objective flag, unit cylinders, five-byte pixels, no-frame row, timing harness |
| `WingLadderTest.cpp` | 171 | 8 | the wing (dungeon) ladder: steps on probes alone, never steps back on a lower score, collapse alarm fires once after 3 low reads and clears, first rung warns after 5, follower takes the host's rung |

(The `Tests` column counts `TEST*` macros found by grep; "+" marks files where the name list was cut off in my listing,
so the real count is higher: UNVERIFIED exact counts for those.)

**`LiveLayoutPinTest` and the golden.** `LiveLayoutPinTest.cpp` builds every live stage's layouts with no database and
prints/compares a text form; `LiveLayoutPin.golden.inc` holds the expected text (`R"PIN(...)PIN"`). Per stage and per
block
it records name, BlockId, revision, and for blocks that do not read the class catalog (everything except core, duel,
pet) obs/action counts, column names, action names and a hash; the class list is pinned too. To regenerate: run
`unit_tests` with `ANIMUS_PIN_PRINT=1 --gtest_filter=LiveLayoutPinTest.*` and paste the printed text into the `.inc`
(`LiveLayoutPinTest.cpp:36-58`, comment; `:216`, the switch; the comparison is `EXPECT_EQ` at `:218`). The file's own
rule: "do not edit the golden to make a test pass": a change is a
change to every checkpoint of the stage. The `.inc` has a block for each of the 12 live stages, move1_controls to
dungeon3_deadmines.

## 2. pytests

Torch: the 11 files marked "stdlib" import neither torch nor `animus`: `test_conf_covers_tuning`, `test_conf_prune`,
`test_distill`, `test_forgectl`, `test_run_snapshot`, `test_spec_builds`, `test_stage_json_diff`, `test_stage_names`,
`test_stage_purpose`, `test_stage_ticks`, `test_stage_validation`. The others import `animus` modules; 37 import torch
directly (`importorskip("torch")` skips in `test_cast`, `test_cast_vision`, `test_masking`, `test_parallel`,
`test_partners`, `test_stage`, `test_top_rung_convergence`, `test_train_run`). "Src" = reads C++ sources/docs/configs
through `parents[4]`.

GPU-only tests (skip when `torch.cuda.is_available()` is false): all of `test_rollout_graph.py` (module `pytestmark`,
line 11), one test in `test_free_look.py` (line 353), one in `test_vision_encoder.py` (line 477), the fused-GRU tests
marked `requires_gpu` in `test_recurrent.py` (line 399; they also call `.cuda()` at lines 372-380). Slow marker
(excluded by default): three tests in `test_parallel.py` (lines 40, 109, 137) and three in `test_train_run.py` (lines
276,
369, 417). Machine-state skips: `test_conf_prune.py:381` (needs the `pre-cleanup-2026-10-07` tag in the checkout),
`test_m1_sight.py:167` (needs an M1 checkpoint; `FORGE_M1_CHECKPOINT` or the default
`/azerothcore/var/animus-forge/shared/runs/_finetune/move1_controls/best.pt`), `test_stage_json_diff.py:172` (needs a
move2_seek `stage.json` from the live run or the backup), `test_resume_check.py:313` (needs the M2 backup checkpoint),
`test_human_reader.py:210` (needs `capture-sample.bin`; `ANIMUS_CAPTURE_SAMPLE` overrides),
`test_stage_validation.py:84`
(needs `ANIMUS_COMPILE_DB`, a configured build's `compile_commands.json` for this checkout, to compile `Stages.cpp`).

### Learner core (networks, training, protocol)

| File | Lines | Covers | Needs |
|---|---|---|---|
| `test_async_sync.py` | 228 | `animus.async_sync`: the vector, mixing rule, leader and follower trading over a real socket | torch |
| `test_bootstrap.py` | 422 | `animus.bootstrap` seeding by name: matching earlier stage on its inputs and actions, missing layout refused, restricted stage overlay, critic not copied, per-block seeding, changed block seeded fresh, init_from chain, finetune_from, catalog that lost a spell, core feature counts vs `CoreBlock.h` | torch; Src (line 279) |
| `test_cast.py` | 102 | frozen checkpoints in declared cast seats (`animus.cast`) | torch |
| `test_cast_vision.py` | 442 | camera-era frozen checkpoints in cast and partner seats: rebuilt from their stage.json, fed camera bytes, refused when the stage cannot feed them, memory cleared per episode | torch |
| `test_casting_weights_equivalence.py` | 176 | `casting_weights` without a baseline equals the function the deployed build ran (cleanup 91811bba6), over every yaml in `configs` | torch via animus |
| `test_class_models.py` | 89 | one model per class but every build still measured on its own | animus |
| `test_config.py` | 51 | config loading rejects wrong-typed values; `resolve_device` falls back (cuda:1 to cuda:0 etc.) | torch (monkeypatched cuda) |
| `test_config_unknown_keys.py` | 28 | unknown keys dropped (and named) from a checkpoint's saved config, an error in a yaml | animus |
| `test_cost_ladder.py` | 157 | the cost ladder climbs on plateau, steps down, holds convergence and LR anneal until full price | animus |
| `test_distill.py` | 1 | empty file (see Observed issues) | none |
| `test_evaluation.py` | 562 | seeded evaluation against a fake sim, convergence tracking, config overrides, every shipped config loads alone and under the fast overlay | animus; configs |
| `test_explore.py` | 63 | Go-Explore starts (`animus.explore`): cells, archive choice, `EXPLORE_STARTS` on the wire | animus |
| `test_export.py` | 195 | `.amdl` export: each layout as the same MLP, model names, empty mask fallback, dims, manifests, observation stats folded into the adapter, recurrent actor with goals | torch |
| `test_export_seat_sets.py` | 190 | `.amdl` versions 7/8/9: sets and attention travel in the file, reference forward matches the learner, goldens | torch; golden files |
| `test_foresight.py` | 29 | foresight targets at terminals and truncations | animus |
| `test_gae.py` | 73 | GAE against hand-computed values | animus |
| `test_gates.py` | 167 | shipped configs against the columns the curriculum emits (parses C++) | Src |
| `test_goal_queue.py` | 300 | two goals and a queue: promotion, ended secondary dropped, director's primary, slots on the wire, rollout and update with four slots | torch |
| `test_goal_targets.py` | 126 | goals as kind and target, masked by the sim's goal block, same in exported models | torch |
| `test_goals.py` | 93 | the goal head on its own clock | torch |
| `test_golden_update.py` | 168 | the live learner's shapes and numbers after one update, pinned (see goldens) | torch; fixtures; golden |
| `test_layout_metrics.py` | 55 | per class/build training metrics | animus |
| `test_layout_revisions.py` | 49 | a re-laid block refuses resume and seeds fresh | animus |
| `test_masking.py` | 169 | masked sampling never picks a disallowed action; a trainer runs one update | torch |
| `test_normalisation.py` | 115 | when observation normalisers move during the recurrent update; rollout copies carry them | torch |
| `test_outcome_score.py` | 126 | evaluation judged on Outcome and Cost terms; reward audit categories | animus |
| `test_parallel.py` | 155 | two learner processes training one run against the fake sim | torch; **slow** (3 tests) |
| `test_partners.py` | 364 | co-op partners in party seats and evaluation arms (`animus.partners`) | torch |
| `test_progress.py` | 81 | `progress.json` flat, non-finite nulled, evaluation restored after resume, per-class convergence | animus |
| `test_protocol.py` | 331 | protocol encoding and a lock-step exchange with a fake sim over a Unix socket | animus |
| `test_realism.py` | 193 | movement realism in evaluations (`animus.human.realism`, `eval_motion.npz`) | animus |
| `test_recurrent.py` | 475 | recurrent actor: memory carried and cleared, learned from sequences, fused GRU | torch; GPU for 4 tests |
| `test_rewards.py` | 43 | reward audit findings (shaping as objective etc.) | animus |
| `test_rollout_graph.py` | 135 | rollout decisions as a captured GPU graph equals the eager path | **GPU** (whole module) |
| `test_rollout_graph_log.py` | 65 | the graph log line (graph class replaced by a stand-in) | torch, CPU |
| `test_run_logger.py` | 52 | resuming a run whose columns changed rotates the file | animus |
| `test_runs.py` | 104 | checkpoint rotation, archive beside runs, resume checks, per-rung best copied | animus |
| `test_seat_sets.py` | 193 | entities as sets (`mappo.seat_sets`): off = as before; on = order independent, pointer actions follow the entity | torch |
| `test_seed_from.py` | 71 | which checkpoint seeds the next stage (`seed_from`) over every config | animus |
| `test_sight.py` | 287 | the sight list beside the camera's entity list, pointer heads, seeding | torch |
| `test_sil.py` | 78 | self-imitation (`mappo.sil`) | torch |
| `test_span_gae.py` | 42 | slow-clock GAE credited on its own clock | animus; helper `slow_gae_reference.py` |
| `test_stage.py` | 209 | `ConvergenceController` rules that end every stage | torch |
| `test_style.py` | 282 | movement-style reward (`animus.style`) | torch |
| `test_tick_split.py` | 50 | world tick vs decision: the split on the wire (`tick_ms`, `decision_ticks`) | animus |
| `test_top_rung_convergence.py` | 120 | a ladder stage converges only at its top rung | torch |
| `test_train_run.py` | 465 | a whole `TrainingRun` against a fake sim: updates, evaluations, checkpoints, finish | torch; fixtures; **slow** (3 tests) |
| `test_two_clock.py` | 118 | two-clock seat and predictions; a full rollout-and-update cycle | animus |
| `test_update_stats.py` | 77 | the recurrent update's reported statistics vs `update_stats_reference.json` | torch; reference |
| `test_rung_rebaseline.py` | 330 | a gate-stepped ladder re-baselines stage convergence at every rung; reads `train.py` source (line 326) | animus |
| `test_shaping_fade.py` | 373 | the shaping fade steps on evidence, survives resumes, reaches the sim | animus; configs |

### Vision, perception, layout

| File | Lines | Covers | Needs |
|---|---|---|---|
| `test_compass_split.py` | 171 | compass split: an M1 checkpoint at move revision 4 seeds the new layouts | torch; Src |
| `test_free_look.py` | 575 | free-look heads, wire (SPEC LookHeads, ACT look), PPO ratio and entropy with the look | torch; 1 GPU test |
| `test_mental_map.py` | 308 | the map crop on the wire, decode vs the sim's encoding, map encoder, seeding | torch |
| `test_move_controller_layout.py` | 83 | move block revision 2 and core revision 1: old checkpoints refused on resume, seed fresh | torch; Src |
| `test_party_frames_seeding.py` | 290 | PartyFrames revision 2 and what seeding carries | torch; Src |
| `test_vision_bytes.py` | 303 | camera frames as bytes (protocol 21/23), decode table, no-frame pattern, cross-check with C++ constants (ported as written) | torch |
| `test_vision_encoder.py` | 580 | camera encoder: per-layout vision spans, device decode, class embedding, shared actor/critic encoder, export refused with a camera | torch; 1 GPU test |
| `test_vision_identity.py` | 148 | slot byte, class and type embeddings, five-byte images round trip | torch |
| `test_heldout.py` | 45 | a held-out arena is named against stage.json and pinned on the wire (protocol 18) | animus |

### Stages and configs (read the C++ sources and yaml)

| File | Lines | Covers | Needs |
|---|---|---|---|
| `test_combat_stages.py` | 294 | combat1-3 configs load and read their measures, ladders step on their gate with full prices, seed chain from M2 | torch; Src |
| `test_conf_covers_tuning.py` | 49 | every `CurriculumTuning::Visit` key is in conf.dist and every `AnimusForge.Curriculum.*` key in conf.dist is read | stdlib; Src |
| `test_dungeon_stages.py` | 244 | G2/D1-D3 configs, fixed-seed evaluation and videos, seed chain, Wailing Caverns held out; `collect-videos.sh --dry-run` output | animus; Src |
| `test_group1_roles.py` | 173 | G1 config, ladder, evaluation, seed from combat3_survive with move4_follow merged, partners never take the drilled seat, readings weighted by episodes | animus; Src |
| `test_interact.py` | 294 | M3 config, ladder, seed from M2, the sight block's named row | torch; Src |
| `test_m1_sight.py` | 207 | M1 config measures, per-compass rates, fade/cost-ladder gates, seeding from the earlier M1 checkpoint | torch; Src; checkpoint skip |
| `test_manual.py` | 96 | the budget table in `docs/forge/04-curriculum.md` matches every `configs/*.yaml` (`total_env_steps`, `every_env_steps`, episodes) | animus; **reads the doc** |
| `test_metric_names.py` | 241 | every metric a yaml names is one its stage's sim or the learner produces (`sim_metrics.py`); recorded M2 `episode_info` | animus; Src; fixture |
| `test_move4_follow.py` | 107 | M4 config, ladder, per-event columns, seeding from M2 | torch |
| `test_seek_metrics.py` | 101 | M2 seek's measures as the training means and tables read them | animus |
| `test_spec_builds.py` | 61 | every standard talent build in `SpecBuilds.cpp` reaches its last row (71 points) | stdlib; Src |
| `test_stage_names.py` | 140 | every `stage|move<N>_name` token written in `apps/forge/python`, `apps/forge/models`, **`docs/forge`**, `worldserver.conf.dist` and the sibling module exists in `Stages.cpp` or is an archived/dated citation; configs match stages | stdlib; Src |
| `test_stage_purpose.py` | 160 | each stage names the term its purpose is paid as and it is an Outcome or Cost | stdlib; Src |
| `test_stage_ticks.py` | 66 | every movement stage ticks at 50 ms (`Stage.<name>.TicksPerDecision`) in conf.dist | stdlib; Src |
| `test_stage_validation.py` | 101 | `Stages.cpp` passes the sim's own validation (compiles it alone and runs it) | stdlib; `ANIMUS_COMPILE_DB`; a C++ compiler |
| `test_status_headline.py` | 65 | `status.headline` reaches the sim through progress.json | animus |

### Human capture (`animus.human`)

| File | Lines | Covers | Needs |
|---|---|---|---|
| `test_human_cli.py` | 116 | `python -m animus.human` commands on a synthetic hour | animus |
| `test_human_fit.py` | 109 | the controller emulator and the beam search recover a known action sequence | animus |
| `test_human_mapper.py` | 94 | movement mapped back through the emulator, casts, selections; reads `MoveControls.h` | Src |
| `test_human_motion.py` | 80 | one definition of motion features | animus |
| `test_human_parity.py` | 286 | companions vs human players on synthetic clients | animus; helper |
| `test_human_reader.py` | 308 | capture framing, record types, gzip members, truncation | animus; helper; optional sample |
| `test_human_tracks.py` | 179 | tracks to segments to dataset/reference/trips/hard spots | animus; helper |

### Tools and forgectl

| File | Lines | Covers | Needs |
|---|---|---|---|
| `test_conf_prune.py` | 397 | `conf_prune.py` (unknown keys, prune/restore, ssh via a fake runner, the conf.dist diff between the tag and HEAD) | stdlib; Src; git tag |
| `test_forgectl.py` | 1672 | 129 tests of forgectl: config parse, audit, console parsing, locks, stage, deploy/move-host, conf-sync, logs, test command parsing, the shim, cluster.toml values | stdlib; `cluster.toml` |
| `test_resume_check.py` | 318 | `resume_check.py` on a tiny fixture checkpoint and the stage.json fixtures | torch; fixtures |
| `test_run_snapshot.py` | 74 | `run_snapshot.py` readings and comparison | stdlib |
| `test_stage_json_diff.py` | 207 | `stage_json_diff.py` kinds and flags | stdlib; one machine-state skip |

## Goldens and fixtures

| File | Pins | Regenerate |
|---|---|---|
| `golden/learner_update.json` | state-dict keys/shapes and the update's statistics and parameter checksums after one update, for `move2_seek` and a synthetic `tiny_layouts` | `python tests/test_golden_update.py write` in the dev container (`test_golden_update.py:166-167`), only for a change meant to move the learner |
| `update_stats_reference.json` | the recurrent update's reported statistics | call `write_reference()` from `test_update_stats.py:65` by hand (no command line entry); last regenerated 2026-10-07 per its docstring |
| `golden/seat_attention.{amdl,json}`, `seat_attention_recurrent.amdl` | version-9 model bytes and logits the in-game reader (`mod-animus`) is checked against | delete the three files and run `test_the_golden_vectors_are_current`, which writes them when `seat_attention.json` is missing (`test_export_seat_sets.py:165-172`) |
| `golden/seat_sets.{amdl,json}`, `seat_sets_recurrent.amdl` | version-8 vectors, frozen | not regenerated by any code found; UNVERIFIED how they were made |
| `LiveLayoutPin.golden.inc` | the live stages' layouts | `ANIMUS_PIN_PRINT=1` (see section 1) |
| `fixtures/seek_stage.json` and `fixtures/stage_move2_seek.json` | the real M2 `stage.json`, "copied from the 2026-10-07 backup" | identical files (byte for byte; `cmp`); two copies kept: `seek_stage.json` is used by `test_golden_update.py`, `stage_move2_seek.json` by `test_resume_check.py` |
| `fixtures/seek_spec.json` | the spec the M2 checkpoint saved | copied from that checkpoint; UNVERIFIED exact provenance |
| `fixtures/stage_move1_controls.json` | a real M1 `stage.json` | "real stage.json files, copied out of the live run's backups" (`test_resume_check.py:218`) |
| `fixtures/recorded_m2_episode_info.json` | the 235 `episode_info` names build 64b7c7dc5 wrote for move2_seek | recorded from a run; used by `test_metric_names.py:137` |
| `fixtures/test_stage.yaml` | the tiny stage config the learner tests run on | hand written |

## What the tests do not cover

- **The GPU rollout graphs.** `mappo.rollout_graphs` capture is exercised only by GPU-skipped tests
  (`test_rollout_graph.py`, one test each in `test_free_look.py` and `test_vision_encoder.py`); the learner trim edited
  that code
  and `forgectl test` (CPU) skips all of them. Only the log-line test (`test_rollout_graph_log.py`) runs on CPU.
- **The device renderer on a real GPU.** `VisionGpuDataTest` needs data and a device; in the standard run it skips.
- **Real cluster paths.** `ClusterLink`, `DealClusterLearners`, `WorkerPlan`, the fingerprint exchange and the
  distributed learner (`animus.parallel` is covered by two-process tests marked slow, over a fake sim) have no GTest;
  the
  nothing else covers the cluster. `forgectl` is tested against fakes: ssh, docker and the real console are never
  involved.
- **forgectl write paths on real machines.** `conf-sync` writes, `move-host`, `build --cluster` are tested with faked
  command runners (`test_forgectl.py`), not against a bind-mounted conf or a real container.
- **`ForgeConfig::Load`.** No test loads `ForgeConfig`; the conf template is only checked textually
  (`test_conf_covers_tuning.py`, `test_stage_ticks.py`) and through `conf_prune.py` tests.
- **The Python learner against the real sim.** Fake sims only; the wire format is checked against hand-ported constants
  (`test_vision_bytes.py`, `test_protocol.py`) rather than against the C++ headers (the same constants are copied by
  hand,
  so drift is caught only where a test reads the header text, e.g. `CoreBlock.h`, `MoveControls.h`).
- **Slow tests are not run by `forgectl test`** (`addopts = -m 'not slow'`).
- **Tools without tests:** `forge_classes.py`, `rename_runs.py`, `cluster-pull.sh` (named only as a string in a fake),
  `spec_builds/*`, `gen_config_reference.py`, `patches/amdl8-check/*`.

## Observed issues

- `tests/test_distill.py` is a one-line empty file: dead (`animus/distill.py` still exists, check
  [py-learner.md](py-learner.md) for whether it is used).
- `test_manual.py` makes `docs/forge/04-curriculum.md` a test fixture: deleting or renaming that document, or its
  budget table, fails the suite. `test_stage_names.py` scans every `.md` under `docs/forge` (including
  `docs/forge/reference/`), so a document that writes an old `stage<N>_<name>` token not in its `ARCHIVED` list and not
  on a dated line, or a token for a stage that does not exist, fails the suite. Both couple the docs to the tests.
- `fixtures/seek_stage.json` and `fixtures/stage_move2_seek.json` are identical (148,174 bytes each).
- `test_golden_update.py` and `test_update_stats.py` regenerate goldens by different, undocumented-in-CI mechanisms (a
  `write` argument versus an uncalled function).
- `CompassBlockTest.cpp:77` skip condition ("move2_seek is not defined yet") is dead.
- `test_m1_sight.py` defaults its checkpoint to a path under `/azerothcore/var/animus-forge/shared/runs/_finetune/...`;
  on any machine without it, 1 test skips silently.
- `StockadeRoomsDataTest`/`StockadeHallwaysDataTest`/`DeadminesSitesDataTest` mix verification tests with "authoring"
  scans that generate site tables when an env var is set; authoring code lives in the test binary.
- The `unit_tests` target links `modules` when present and adds module test sources (`src/test/CMakeLists.txt`, the
  `ACORE_MODULE_TEST_SOURCES` blocks);
  with `mod-animus` present and not disabled its tests would join this binary (the dev override disables it).

## Reviewer notes

- Decide whether `forgectl test` should run the slow marker nightly; the slow tests are the only coverage of the
  two-process learner and of end-to-end `TrainingRun`.
- The llvm-17 relink is a workaround for the container image. Fixing the image (installing the compiler-rt of clang 18)
  would remove `forgectl-test.sh:52-60` and the failed-link tolerance at lines 42-50.
- `LiveLayoutPin` pins layouts, `test_golden_update` pins learner numbers: together they are the safety net for the
  refactor. A refactor of blocks or the update must keep both green without regenerating the goldens.
