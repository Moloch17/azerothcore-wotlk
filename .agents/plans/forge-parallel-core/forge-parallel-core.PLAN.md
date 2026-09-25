# Forge core: parallel, memory-resident, vectorised sim host

## Context

The `forge` branch is a purpose-built RL sim host. Until now it kept every change rebasable on upstream
AzerothCore: Forge replacements are called from the top of stock functions whose bodies stay in place but
never run, `Main.cpp` is still in the tree, and nothing upstream-owned is edited. That constraint is being
dropped. The user wants:

1. **No more rebasing.** Upstream code may be edited in place and dead upstream bodies removed.
2. **The core as parallel as possible.** One fully loaded, fully functional world (every system kept
   except Warden) hosting as many bots as fit, across as many simulations as fit.
3. **The database loaded once and then closed.** All state lives in memory; no DB worker threads run
   during simulation.
4. **Vectorise / SIMD** wherever the data is dense.
5. **GPU** used for massive parallelism where it pays.

What the surveys established (numbers are the 2026-09-17 measurement, 128 envs, 250 game-ms decisions):

| Per decision, 24 ms | ms | Where it runs |
|---|---|---|
| World tick (all maps) | 1.8 | map threads |
| Module reset | 6.4 | world thread, serial |
| Module observe | 3.3 | world thread, serial |
| Module apply + reward | 2.0 | world thread, serial |
| Learner round trip (3 ms compute) | 10.1 | Unix socket, blocking |

So the map update is 7% of the wall clock. Ordering below follows the budget: first move the serial
module work and the learner exchange off the critical path, then scale the map update itself for
thousands of bots per map, then GPU.

Facts that shape the design:

- World data is already memory-resident after startup (ObjectMgr/SpellMgr/DBC are C++ structures; there
  are no lazy world reads). What still touches MySQL is `CharacterDatabase` persistence: instance
  create/destroy (3 sync SELECTs + ~5 async writes per env per episode), corpses, boss/instance saves,
  `log_encounter`, DB-spawn respawn times, game-event state, mail/AH/guild writes, and a sync `KeepAlive`
  ping burst every 10 game-minutes. Every async call funnels through `DatabaseWorkerPool::Enqueue`, every
  sync call through `GetFreeConnection` (`src/server/database/Database/DatabaseWorkerPool.h:219-227`).
- Parallelism today is one thread per `Map` (`MapUpdater`, mutex queue, heap request per map per tick).
  Continent stages put every env on one base map separated by phase (`1 << (1 + env.Index % 31)`,
  collides past 31 envs), so continent stages are single-threaded. The module runs entirely on the world
  thread inside `OnWorldUpdate`, then blocks on the learner.
- Hot-path layouts: grid cells are intrusive linked lists per type; every range query is a cell-rectangle
  list walk. `UpdateMask` is one byte per field. `Unit::m_modAuras` is 317 empty `std::vector`s (7.6 KB per
  unit). `GetTotalAuraModifier` builds a `std::map` per call (~20 calls per melee swing). `resetMarkedCells`
  memsets 32 KB per map per tick. Build is `-O2`, no `-march`, no LTO.
- Hardware: Ryzen 9 9950X3D (16c/32t, AVX-512, 96 MB V-cache on cores 0-7), 30 GB RAM, Radeon 7900-class
  (gfx1100). `/dev/kfd` exists; no ROCm on the host; the worldserver runs in a container whose compose file
  already has a commented AMD passthrough block. Learner: `train_device` auto (ROCm-capable),
  `rollout_device` cpu.
- Fully loaded world: ~150k creature + ~97k gameobject spawns, ~3.5k continent grids. Estimated 6.5-7 GB
  RSS (5.5 GB after the aura-array fix), plus 100-150 KB per bot. 30 GB fits one world, 4-8k bots and
  2-3 replicas of the largest continent, provided the MySQL container is stopped after load.

**GPU architecture: the device owns the continuous world, the CPU owns discrete events.** The user needs
the GPU used as much as possible. The split that makes that real:

- **Static map data lives on the device**, uploaded once at load and read-only after: terrain heights and
  liquid (V8/V9 arrays, ~350 MB for every continent), the VMAP collision trees flattened to BVH node +
  triangle buffers (~600 MB), and the Detour navmesh tiles flattened to polygon/vertex/link buffers
  (~1.3 GB). All fit in the card's 20 GB alongside the learner.
- **Dynamic unit state lives on the device** as SoA buffers (positions, orientation, velocity, health,
  power, flags, phase, faction, timers, visibility radius) for every unit on every map, mirrored from
  the CPU objects by dirty deltas each tick.
- **The continuous part of a tick runs as a device pipeline**: movement integration and spline advance →
  ground/water snap (terrain) → line of sight (BVH) → visibility pairs, range/aggro/threat proximity →
  navmesh path queries for every unit that needs one → regen and simple periodic ticks → observation
  encoding straight into a device tensor → rollout inference on the same card → actions back as a small
  buffer. Observations never leave the GPU, which removes the learner round trip.
- **The combat core as device tables** (Phase 7): auras, per-unit modifier sums, cast requests and
  outcomes, threat, cooldowns and the stats formula become row tables with a kernel each, fed by the
  spell/faction/level-stat tables uploaded at load. The existing core is the differential-test oracle.
- **What stays on the CPU**: hand-scripted spells and C++ creature AI (flagged as exceptions in the
  spell table), and the rare life systems: inventory, loot rolls, quests, professions, mail, auction,
  guilds. They consume the device's results and emit discrete events that the delta sync carries back.

At today's scale (128 envs, ~600 units) the device round trip costs more than the 1.8 ms tick it
replaces; the design wins at thousands of bots per map, which is the target. Every device kernel keeps an
AVX-512 CPU twin as the correctness oracle and for the CPU-exception path; once state is device-resident
the device is the default for every data-parallel step, since the inputs are already there and PCIe is
only crossed by the per-tick delta.

## Status (2026-09-25, branch `worktree-forge-parallel-core`, core repo only)

Delivered and syntax-checked (no full build; the user builds with `./forge.sh --build`):

- **Phase 0** done: fold of the call-at-top pattern, `Main.cpp` deleted, Warden kept, `-O3 -march`,
  LTO (`ConfigureLTO.cmake`, needs the `llvm` package now in both Dockerfiles), PGO knobs, Release
  default, core `Map::UpdateTiming` per phase summed on `MapMgr`.
- **Playtest mode** done: `Forge.Playtest` (`src/server/game/Forge/Forge.{h,cpp}`), wall clock,
  listener, realm info, real-time loop, `Forge::HasClients()` gating 27 packet sites.
- **Phase 1** done in staged form: `DatabaseWorkerPool::Seal(strict)`; `ForgeSealStrict = false` in
  `ForgeMain.cpp` until the module warms its lazy caches (sync reads are served and logged as
  stragglers). Sync-read sites guarded, sim instances never reset by the calendar, in-memory manager
  ticks restored.
- **Phase 2** done except items 3, 5, 6, 7, 9: scheduler, uniform ticks, per-map epilogue, cell
  marks, client-gated dirty set, bitset mask, virtual accessor, per-map player index.
- **Phase 2.5** done: the module is core code (`src/server/game/Animus`, `apps/forge`, `docs/forge`,
  keys in `worldserver.conf.dist` with the legacy overlay still read); script hooks replaced by direct
  calls carrying the spell; `WarmCaches()` before the seal; reset split, map timing in `forge status`,
  playtest tick tolerance. Python suite: 610 pass, 10 pre-existing model-JSON stage-name failures
  (identical in the old module checkout). Still staged: `ForgeSealStrict = false` until one full run
  logs no straggler; `mod-animus.cmake`'s exclusion check lives in that module's own repo.
- **Phase 3 item 1** done (syntax-checked, unmeasured): a decision is `BeginDecision` on the world thread
  before the maps, `ApplyActionsForMap` + `ObserveMap` inside each map's task (`MapUpdater::RunMapTick`),
  and `FinishCollect` on the world thread after the join, which keeps only the ended episodes' info and
  their reset. The `forge status` figure for observe, reward and apply is now thread time inside the world
  number, not on top of it.
- **Phase 3 item 2** done (syntax-checked, unmeasured): continent replicas as child `Map` objects of the
  base continent, `AnimusForge.ContinentReplicas` sizing them, envs dealt in contiguous blocks of at most
  31 so the phase formula is unchanged. A continent stage is now as many map tasks as it has replicas.

Deferred, with reasons:
- `_valuesUpdateCache` removal (no cost while empty), templated aura scans, `std::list` target lists,
  sparse `m_modAuras`: need a real build and a bench run to justify; the `RegenerateAll` guard is
  commented out upstream too, restoring it changes regen dynamics, left alone.
- Strict seal + stopping MySQL after startup: after the module warms caches and one full sweep logs
  no straggler.

## Throughput estimate (unmeasured; ±30%; anchored on 2026-09-17: 24 ms/decision at 128 envs)

At 128 envs: Phase 0 ~21.5 ms (6k env steps/s); Phases 1-2.5 ~21 ms (DB/hook savings are
sub-millisecond, uniform ticks raise the world tick from 1.8 to ~3 ms); Phase 3 ~14 ms (9k) as the
11.7 ms serial module block spreads over 16 threads; Phase 4 ~5-6 ms (22-25k) with the shared-memory
exchange and half-batch overlap; Phases 5-7 add nothing at this size (the device round trip costs
about what it saves at ~600 units). Whole plan at today's scale: about 4-5x.

At ~5,000 units per host (e.g. 8 replicas x 250 bots plus opponents): today's code extrapolates to
~150 ms/decision (~33k unit steps/s: serial module ~100 ms, exchange ~40 ms, visibility super-linear);
the end state with the device pipeline and combat tables is ~5-8 ms (~600k-1M unit steps/s), a 20-30x
gain per host, multiplied near-linearly by cards under the map-ownership partition. Every phase's
`forge status` line replaces these numbers with measurements.

Saturating this machine (asked explicitly): RAM allows ~15-20k bots (7 GB world + ~9 GB for 20
continent replicas + ~125 KB per bot); the GPU pipeline, buffer and PPO updates for that many units are
not the limit on one card; the CPU is. With the combat tables in place the CPU keeps ~3-5 us per unit
per tick of exception work, so ~40k units cost ~8-13 ms per tick on 16 cores: ~75-125 decisions/s,
~2M env steps/s, game time 20-30x wall clock. Everything hangs on that per-unit CPU cost (at 20 us,
divide by five), so Phase 7's coverage line is the number to watch. Extra GPUs add learner headroom,
not sim throughput, while the CPU is the wall.

## First build, 2026-09-25 (revision a8171f29855c, `forge` fast-forwarded to this branch)

The stack compiles and runs. `forge` was fast-forwarded onto this branch after aborting an unfinished
merge of upstream master (via `worktree-curriculum-rework`) that had been left mid-conflict in the main
checkout with 30 files unresolved.

What the first build found, in order:

1. **Duplicate symbols at link.** The checkout at `modules/mod-animus-forge` was still built as a module
   and every folded symbol existed twice. `modules/CMakeLists.txt` now drops a module whose sources are
   part of the core and says so at configure time.
2. **Link-time optimisation was inert, and the cause was not the probe.** Fixed; the binary now has it.
   Three layers, each hiding the next. `check_ipo_supported` builds its test through the *project* form
   of `try_compile`, which forwards a fixed set of variables and not the archiver clang needs, so it
   linked with `CMAKE_CXX_COMPILER_AR-NOTFOUND` and declared the toolchain incapable;
   `CMAKE_TRY_COMPILE_PLATFORM_VARIABLES` does not reach it, because that applies to the source-file
   form. LTO is now enabled on this file's own `find_program` check for clang, and the probe is kept
   only for other compilers, where nothing had to be found by hand. With LTO actually on, the *real*
   archive steps then failed the same way: `project()` loads the compiler detection CMake stores under
   `CMakeFiles/<version>/`, that file does a plain `set()` of the archiver, a normal variable shadows a
   cache entry, and the build volume's copy still said NOTFOUND from before the image had llvm. The
   archiver is now also set as an ordinary variable ahead of every `add_subdirectory`, and the stale
   detection in the volume was deleted once so CMake re-detected it.
3. **The build type is now Release.** It came from `conf/dist/env.ac`, which the container reads as its
   `env_file` and which overrode the Release default in `config.sh`. `ASSERT` is the core's own macro
   rather than the standard `assert`, so Release keeps every check. The binary went from 362 MB to
   51.8 MB, and the whole build including the LTO link takes about a minute and a half warm.
4. **47 `Missing property AnimusForge.*` warnings.** The live `env/dist/etc/worldserver.conf` predates the
   fold, so the keys the fold moved into the template are absent from it; the legacy
   `etc/modules/mod_animus_forge.conf` is still read afterwards and still wins, so the values in force are
   correct and the warnings are noise. Regenerating the live conf from its `.dist`, or dropping the legacy
   overlay, clears them.

What ran clean: world load, all three pools sealed (staged), no synchronous-query straggler at startup,
the host's status block printing the folded paths, SOAP listening, the forge idle. The GPU reaches the
container and the learner's torch sees `gfx1100`, 20 GiB.

Still unmeasured: the per-decision `forge status` line every phase is supposed to record needs a training
run, which memory `feedback-no-smoke-until-stages-added` gates on the user.

## First measurement, 2026-09-25 (revision 7558137fa, `forge bench` on `stage8_duel`, policy `random`)

The user granted a one-off permission for this run, overriding memory `feedback-no-smoke-until-stages-added`.
16 trials, no abort, no scripted policy. The previous attempt aborted at spawn time; two fixes had to
land first, both recorded in the commit above.

**Sim only, no learner** (env steps/s; `world` is the map update, `sim` is observe + apply + reset):

| threads | 64 envs | 128 envs | 192 envs |
| --- | --- | --- | --- |
| 4 | 16,300 (world 3.7 ms) | 26,561 (4.4 ms) | 25,056 (7.0 ms) |
| 8 | 16,536 (3.6 ms) | 28,337 (4.1 ms) | **36,198 (4.7 ms)** |
| 12 | 12,840 (4.8 ms) | 19,780 (6.0 ms) | 26,716 (6.5 ms) |
| 16 | 12,119 (5.1 ms) | see below | see below |

**With the learner**, the two fastest settings re-run (this is the like-for-like against the anchor):

| threads | envs | torch threads | env steps/s | per decision | world | sim | learner |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 8 | 192 | default | 7,272 | 29.9 ms | 6.3 | 5.2 | 18.4 |
| 8 | 192 | 8 | 7,259 | 29.5 ms | 6.2 | 4.8 | 18.4 |
| 8 | 128 | default | 5,403 | 26.4 ms | 5.6 | 4.1 | 16.7 |
| 8 | 128 | 8 | 5,470 | 26.2 ms | 5.4 | 4.2 | 16.7 |

RSS 1.6 GB at the first trial, 2.3 GB from the fourth on, then flat: replicas and their grids persist
across trials rather than being rebuilt per trial.

What the numbers say, in the order that matters:

1. **End to end, nothing has changed yet.** The anchor is 24 ms per decision at 128 envs on 2026-09-17,
   which is 5,333 env steps/s. With the learner in the loop today it is 26.2 ms, 5,470 env steps/s. The
   whole gain of Phases 0-3 is currently invisible from outside, because the learner exchange, not the
   sim, is the wall.
2. **The sim itself did move, a lot.** World plus sim at 128 envs is now 9.6 ms of the 26.2, and 4.5 ms
   of it without the learner attached. The plan predicted ~14 ms at this point (Phase 3) and ~5-6 ms only
   after Phase 4. The map side is already past its Phase 4 forecast; the exchange is not.
3. **Phase 4 is therefore the next phase, and the plan's own forecast for it is now the whole story.**
   16.7 ms of a 26.2 ms decision is the socket exchange and the learner's own step. Halving it would be
   worth more than everything Phases 5-7 promise at this scale.
4. **Thread scaling is wrong above 8, and CPU inside the map update grows with threads.** 12 and 16
   threads are slower than 8 at every env count. Worse, the summed per-map `world parts objects` thread
   time at 128 envs goes 5.7 ms on 4 threads to 12.4 ms on 8, for flat wall time: the work itself doubles.
   With 31 envs per replica, 128 envs is only 5 map tasks, so threads beyond 8 cannot help, but they
   should not cost. This is a contention or pinning signature and it is exactly what Phase 3's
   verification item (per-map task times in the timing line) was meant to expose. That line does not
   exist yet. **Next measurement: add it, then re-run this sweep.**
5. `forge bench` now reports the fastest setting as 8 threads / 192 envs, 1.0x what the live config runs
   (16 threads / 128 envs), and warns that 192 envs changes the learner's batch, not only the speed.

Claims deliberately not made: the pet guard cannot be observed firing, only that the scenario which
aborted at spawn now completes 16 trials. The gain is Phases 0-3 together, not replicas alone.

## Second measurement, 2026-09-25 (revision 6474ff9d0, same bench, now with map task times)

Same settings as the first: `stage8_duel`, policy `random`, 12 sim trials then the 2 fastest with the
learner. Tasks per update are 6 / 8 / 10 at 64 / 128 / 192 envs (not 5: other maps tick as well). `Parallel`
is summed task time over the update's wall time; `CPUs` is the last update only.

| threads | envs | env steps/s | world ms | longest task ms | parallel | objects ms | CPUs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 4 | 128 | 29,482 | 3.9 | 3.90 | 3.6x | 6.69 | 0-3,24 |
| 8 | 128 | 29,688 | 3.9 | 3.87 | 3.7x | 6.75 | 0-3,25 |
| 8 | 192 | 40,391 | 4.1 | 4.12 | 5.5x | 10.96 | 0-2,4-5,7,25 |
| 12 | 128 | 21,673 | 5.5 | 5.46 | 3.4x | 9.63 | 1-2,6-7,13 |
| 16 | 128 | 20,106 | 5.9 | 5.90 | 3.4x | 10.38 | 0,4,8,15,24 |
| 8 + learner | 192 | 7,530 | 5.7 | 5.68 | 4.7x | - | 0-2,4-5,7-8 |
| 8 + learner | 128 | 5,449-5,508 | 5.4 | 5.34 | 3.3x | - | 0-5 |

Full table: `var/animus-forge/shared/bench/bench.json`.

1. **The map update is one map long.** The longest task equals the wall time in 15 of 16 trials; the one
   exception is 4 threads / 192 envs, where 10 tasks do not fit 4 workers plus the world thread. More
   threads cannot shorten the update below the heaviest map's tick. Next: name that map (the status row
   has it; the bench line does not yet) and see whether it is the base continent or a replica.
2. **Above 8 threads, the same tasks run slower.** At 128 envs the longest task goes from 3.9 ms to 5.5-5.9 ms
   and non-player object time from 6.7 to 9.6-10.4 ms, with the same tasks doing the same work. Finding 4
   stands, now measured over every map.
3. **Other-die pinning fits, but does not explain all of it.** At 12 and 16 threads the slowest task ran on
   CCD1 (8, 13, 24). But at 8 threads the slowest task also ran on CCD1 twice (cpu 9, and 25 = the world
   thread, which is unpinned and runs tasks in `wait()`) at normal speed. So one task on the other die is
   not slow by itself. What changes above 8 is 4-8 more workers, pinned to CCD1, spinning on
   `_next`/`_count` between tasks; that suggests cross-die cache traffic (spinning plus whatever the maps
   write in common) rather than a slow die. Next test to tell them apart: 12 threads pinned to CCD0's SMT
   siblings (0-7,16-23), against 12 spread across both dies.
4. **The learner slows the map update too**: 3.9 → 5.4 ms at 8 threads / 128 envs, and the new CPU sets show
   tasks on 0-5 while torch runs unpinned. The exchange itself is still 17-19 ms of a ~24-26 ms decision, so
   Phase 4 remains the largest lever.

## Third measurement, 2026-09-25: the learner, taken apart (revisions 644f7f3f3..b9e1ea090)

**The "learner ms" was mostly the PPO update, not the exchange.** The sim blocks for the whole update: on
stage8_duel 1.71 s per 128-decision rollout, 13 of the 17 ms per decision. The exchange plus rollout inference is
~3-4 ms. So Phase 4 item 1 (shared-memory ring) is worth ~1-2 ms at most and is no longer first in line.

Two tools now measure the learner without a worldserver, both reproducing the live numbers within a few percent:
`python -m animus.bench_update` (one update on the stage's real networks) and `python -m animus.bench_learner`
(the real TrainingRun against a fake sim process speaking the protocol, `--sim-ms` standing in for the map update).

| change | update s | learner bench env steps/s (serial / overlapped) | forge bench, 8 thr, 128 / 192 envs |
| --- | --- | --- | --- |
| before (sweep 2) | 1.67 | - | 5,449-5,508 / 7,435-7,530 |
| GRU replay as one fused MIOpen call per sequence | 1.29 | 6,642 / 6,657 (overlap broken) | 6,368 / 8,474 |
| + CPU placement (pool on the V-cache die, learner on the other) | 1.29 | - | 6,878 / 9,053 |
| + overlap_updates fixed (it had never overlapped) | 1.29 | 6,650 / 11,150 | not re-run |
| + host syncs out of the update (3,555 -> 444) | 1.18 | 6,896 / 11,594 | 7,067 / 9,278 serial |
| same, `--set overlap_updates=true` | 1.18 | | **11,670 / 15,460** (2.1x the session's start) |

Findings, in order of consequence:

1. **overlap_updates never overlapped.** The first-update wait keyed on "the previous join returned nothing",
   which the wait itself guaranteed for the next rollout, so every update was joined where it was submitted.
   The earlier conclusion that overlap buys nothing measured this bug. Fixed and pinned by a test; the setting
   stays off because one update of policy staleness is a training decision for the user. On, the real-sim bench
   gives 11,670 / 15,460 env steps/s against 7,067 / 9,278 serial (128 / 192 envs): 1.65x.
2. **The recurrent update is launch-bound.** 4 epochs x 8 minibatches x (actor + critic) = 64 sequential GRU
   replays of 128 steps on 16 rows. Stepping GRUCell was ~210k launches per update; MIOpen's fused call still
   launches per timestep internally (~190k), so it only moved the overhead into C++. A persistent Triton kernel
   was tried and lost (one workgroup at 16 rows, 128 dependent steps, fp32 dots on gfx11 have no matrix units,
   12 weight blocks re-read from L2 per step). What would actually shrink it is fewer, larger sequential sweeps
   (minibatches, epochs), which are training hyperparameters, not speed ones.
3. **CPU placement.** The world thread was unpinned and wandered onto the other die; worker k sat on CPU k,
   spreading across both L3s above 8 threads; torch shared the map workers' CPUs. Now: `CpuPlacement` orders
   CPUs by L3 size then cores before SMT siblings, the world thread takes the first, and the learner is pinned
   to the CPUs sharing no core with the pool. The map update with the learner attached is back to its sim-only
   time (4.0 ms at 128 envs).
4. **Rollout inference on the GPU is slower today** (9,365 against 13,790 rollout env steps/s): per decision it
   pays launches and a device round trip for its outputs. It becomes right only when observations are produced
   on the device (Phase 6).

## Fourth measurement, 2026-09-25: inference, threads, replicas (revision 0cb3868fe, overlap on)

Learner side, `bench_learner` at 5 ms of sim: transport + decode is ~0.8-1 ms per decision, so the shared-memory
ring (Phase 4 item 1) is worth under 1 ms. Rollout inference is the learner's cost: act_and_value 2.15 -> 1.76 ms
after folding the rollout copies' normalisers into their adapters (animus.export's fold). What remains is ~42
small matmuls at ~23 us each (OpenMP fork/join on 13-row slices) and categorical sampling. torch_threads 8 stays
(best serial; 4 is within 4% overlapped). GPU inference stays slower until observations are made on the device:
~17 host syncs per decision (per_layout on the device, 6-8 separate `.cpu()` outputs) at ~180 us each.

`forge bench`, stage8_duel, learner phase with overlap on (env steps/s; map update ms):

| continent replicas | 7 thr, 128 sim-only | 7 thr, 192 sim-only | 192 with learner, 7 / 8 thr |
| --- | --- | --- | --- |
| 0 (31 envs a map) | 32,698 (3.5) | 40,599 (4.1) | 15,924 / 15,825 |
| 8 (16 envs a map) | **36,925 (3.1)** | **43,554 (3.8)** | 15,790 / 15,998 |
| 16 (8 envs a map) | 22,529 (5.1) | 29,821 (5.8) | 15,598 / 15,312 |

1. **7 threads beats 8**: the world thread plus 7 workers are exactly the V-cache die's 8 cores; the 8th worker
   lands on the world thread's SMT sibling. The live config runs 16.
2. **8 replicas beat the default on the sim alone** (smaller maps, a shorter longest task); 16 over-split it
   (19 tasks on 8 threads, and total map work up ~40% from per-map fixed costs).
3. **With the learner the setting stops mattering**: a decision at 192 envs is map ~4.2 + sim ~1.0 + learner
   ~6-7 ms, taken in turn. End to end is now learner-bound, which is what Phase 4 item 2 (half-batch double
   buffering) exists to hide.

**Half-batch design note (not started).** The world thread ticks every map with one global clock, and spell
cooldowns read GameTime, so a half cannot simply tick every other world tick. Proposed: world ticks at D/2 (GameTime
advances D/2), and each half's maps tick every second world tick with diff D, so every map advances D per decision
and the global clock D per cycle; the halves are phase-shifted by D/2. Halves must be disjoint sets of maps
(replicas/instances). A half's world time is its longest map, so it wants maps of ~16 envs or fewer: at 192 envs,
8 replicas give 12 maps, 6 per half. Expected at 192 envs: from ~12 ms per decision to ~2 x max(half map ~3 ms +
sim, half learner ~3.5 ms) ~ 7-8 ms. Touches ForgeMain's tick, MapMgr::Update, the module's decision cycle, the
protocol (StepHeader env range), and the learner's env/buffer/rollout loop.

## Ground rules

- Read `.agents/docs/cpp-guidelines.md` before C++ work; `.agents/docs/build.md` before any build.
  No full build unless asked; each batch is checked with the syntax-check tree (`var/syntax_check.py`,
  see memory `animus-syntax-check`).
- No config gates or feature flags on `forge` (memory `forge-fork-no-gating`): replacements are
  unconditional. New Forge code lives in `src/server/game/Forge/`; upstream files are edited in place.
- Keep every game system functional (guild, mail, AH, LFG, achievements, calendar, tickets, channels,
  instances, battlegrounds, game events). Remove Warden only.
- On approval, mirror this file to `.agents/plans/forge-parallel-core/forge-parallel-core.PLAN.md` and
  keep it updated with per-phase measurements.
- Every phase ends with a `forge status` per-decision line recorded against the baseline above.
- **No scripted baselines, ever.** All behaviour is discovered by the learner. Smoke runs, comparisons
  and plumbing checks use a learner-driven run (a short training run, a random policy for plumbing, or
  an exported checkpoint), never the scripted `fight`/`life` policies or scripted opponents.

## Phase 0: ditch the rebase discipline, add the free layer and timers

1. **Collapse the call-at-top pattern.** Fold `ForgeUpdate` bodies into `World::Update`,
   `MapMgr::Update`, `MapInstanced::Update`, `Map::SendObjectUpdates`, `GameTime::UpdateGameTimers`;
   delete the dead upstream bodies and the `// Forge:` "left in place for rebases" comments. Delete
   `src/server/apps/worldserver/Main.cpp` (`ForgeMain.cpp` is the entry point). Keep `Forge*.cpp` files
   as the home of new subsystems.
2. **Keep Warden.** It stays in the build and loads as stock; `Warden.Enabled` governs it. In sim mode
   every session is a sim session with no socket, so Warden never runs; in playtest mode (below) it
   checks the real client as on a stock realm.
3. **Compiler flags** in `src/cmake/compiler/gcc/settings.cmake` and `clang/settings.cmake`: `-O3`,
   `-march=native` (znver5 needs GCC 14 / Clang 18 in the container image; znver4 fallback keeps
   AVX-512), `-flto=auto`, `-fno-semantic-interposition`; `CMAKE_BUILD_TYPE=Release` for the forge
   image (`apps/docker/Dockerfile`). Link jemalloc (vendored, currently disabled at
   `src/cmake/showoptions.cmake:143`) and drop the glibc `malloc_trim` in `ForgeMapMgr.cpp`.
   PGO as a two-pass build in `apps/docker/forge-worldserver.sh` with `forge bench` as the training run.
4. **Timers.** Split `EnvPool::CollectTiming::ResetNs` into Create / Configure / Place / Destroy; add
   per-map sub-phase timers to `Map::Update` (sessions, players, non-player objects, relocation,
   visibility, delayed) exposed through the same `forge status` line. Metric stays dead.

Deliverable: same behaviour, faster binary, a timing line that can attribute every later change.

## Playtest mode (config switch, the one sanctioned gate)

`Forge.Playtest = 0|1` in `worldserver.conf` (read once at startup, default 0). The user wants a real
player able to log in and confirm the world is joinable and behaves correctly. With it on:

- **Clock**: the game clock follows the wall clock (stock `GameTime::UpdateGameTimers`) and the main loop
  sleeps to `WorldServerPort`-style real-time ticks (stock `WORLD_SLEEP_CONST`), instead of the fixed
  synthetic diff of `ForgeMain.cpp:157-202`. The module still steps its envs on the same ticks.
- **Network**: `ForgeMain` starts the world listener (`WorldSocketMgr`, `WorldServerPort`) and
  `sWorldSessionMgr->UpdateSessions` runs in the world tick; the authserver in `docker-compose.yml`
  serves the login. Warden is enabled per `Warden.Enabled`.
- **Database**: the Phase 1 seal is skipped, so a real login can read `account`, `characters` and
  friends, and the player's saves work as stock. Sim bots keep writing nothing (their guards are
  per-session, not per-mode).
- **Packets**: the `// Forge: no client sockets` early returns (about 40 sites in `Object.cpp`,
  `Unit.cpp`, `Player.cpp`, `PlayerUpdates.cpp`, `Spell.cpp`, `SpellAuras.cpp`, `MoveSplineInit.cpp`,
  `Battleground.cpp`, `Map::SendObjectUpdates`) become per-session: skip when the receiving session is a
  sim session, build when it has a socket. With no real player connected the cost is the same as today,
  because every candidate receiver is a sim session and the check is one flag.
- **Device pipeline**: unchanged; a real player is a unit row like any other, and their packets are
  built from the CPU objects after the per-tick delta apply.

Everything else in this plan is unconditional. This is the only config gate on the forge (recorded in
memory `forge-fork-no-gating` as the exception the user asked for).

## Phase 1: close the database after startup

Seam: `DatabaseWorkerPool<T>` gets a `Seal()` (`src/server/database/Database/DatabaseWorkerPool.cpp`)
called from `ForgeMain.cpp` after `sScriptMgr->OnStartup()` for all three pools (skipped in playtest
mode, where the MySQL container also stays up):

- Joins and destroys the `DatabaseWorker` threads and closes every MySQL connection (both indexes).
- `Execute`, `CommitTransaction`, `AsyncCommitTransaction`, `ExecuteOrAppend`, `DirectExecute`,
  `DirectCommitTransaction`: discard the operation (writes are meaningless once memory is the truth).
- `Query`, `AsyncQuery`, `DelayQueryHolder`, `KeepAlive`, `GetFreeConnection`: **abort with a backtrace**.
  This is the tripwire; a missed sync read would otherwise spin forever in `GetFreeConnection`.

**Invariant: warmed before sealing, sealed before any scenario runs.** The module's world-table queries
are lazy one-shot statics that fire during the first `BuildSeat`, and a later `forge start <stage>`
touches caches the first stage never did (`Supplies.cpp:131`, `GearBuilder.cpp:127/327/392`,
`WorldCreatures.cpp:41`, `Opponents.cpp:119`, `LifeWorld.cpp:204`, `ClassAssets::For` per class). Add a
`WarmCaches()` to the module's startup hook that forces every scenario's static pools and all ten
`ClassAssets`, and call `Seal()` only after it. A future lazy query is exactly what the tripwire catches.

Precondition (verified): `mod-animus` cannot be built alongside `mod-animus-forge` (its cmake fails the
configure), so the live module's `DirectExecute` paths are not in the forge binary.

Then remove the reads the tripwire finds, in memory-only form (functionality kept, SQL gone):

- `Map::LoadRespawnTimes`, `Map::LoadCorpseData` on instance create (`Map.cpp:2545-2564, 3508-3516`);
  respawn/corpse state is already in `Map` members after load.
- `InstanceScript` saved-data read (`InstanceScript.cpp:810-813`): keep the in-memory string.
- The `WUPDATE_PINGDB` block in the world tick.
- `World::ProcessQueryCallbacks` stays (drains an empty processor).
- `InstanceSaveMgr::Update` scheduled resets run on the accelerated game clock and can `InstanceMap::Reset`
  a live instance mid-episode: keep resets, but make `InstanceSaveMgr` skip instances that hold sim
  players (extend the existing `IsSimSession` check at `InstanceSaveMgr.cpp:794`).
- Restore the manager ticks the forge dropped whose state is memory-resident (`ForgeWorld.cpp:31-53`):
  `sAuctionMgr`, `sLFGMgr`, `sOutdoorPvPMgr`, `sWorldState`, `sBattlefieldMgr`, and quest resets. With
  writes discarded they are in-memory systems, which is what "fully functional" needs. Leave the
  row-sweep timers dropped (mail expiry `ReturnOrDeleteOldMails`, expired bans, cleandb): they operate on
  DB rows through sync queries and would trip the wire. `WUPDATE_EVENTS` is a content decision: on the
  accelerated clock holidays cycle in wall-minutes and the director (commit 215811ccc) already drives
  events; restore it only if the director does not, never both.
- Startup: set `WorkerThreads = 1` for all pools in the forge conf (only startup load uses them), and
  stop the MySQL container after the worldserver reports the seal (`apps/docker/forge-worldserver.sh`,
  `docker-compose.yml` healthcheck dependency becomes start-only).

Verification: `ls /proc/<pid>/task/*/comm` shows no `DatabaseWorker`; `ss -tp` shows no MySQL sockets;
the tripwire never fires across a sweep of every stage.

## Phase 2: scheduler and contained hot-path fixes

**Scheduler.** Replace `MapUpdater` (`src/server/game/Maps/MapUpdater.{h,cpp}`) with
`src/server/game/Forge/ForgeScheduler.{h,cpp}`:

- Fixed worker count from `MapUpdate.Threads`; workers pinned, map workers on cores 0-7 (V-cache) and
  their SMT siblings, learner/Python left to cores 8-15 (`taskset` in `forge-worldserver.sh`).
- Per tick a task array built once, atomic index claim, spin-then-park, barrier by atomic generation,
  no per-tick heap. The world thread is worker 0. `activate/deactivate` keep working for `forge bench`.
- Map tasks sorted by last tick's duration (LPT). Each map task runs prologue, update, and its own
  epilogue (`MoveAll*InMoveList`, `HandleDelayedVisibility`, `OnMapUpdate`, `DelayedUpdate`,
  `RemoveAllObjectsInRemoveList`), so the serial `DelayedUpdate` loop in `MapMgr::ForgeUpdate:120-131`
  disappears. Only instance create/destroy and the trim stay on the world thread.
- Drop the 4-step round robin (`mapUpdateStep`, `i_timer[]`, `ForgeMapMatchesStep`): every map gets its
  full diff every tick. Dynamics change: dungeons/BGs currently get their heavy pass every 4th tick with
  4x diff, so instance maps do four times today's per-tick work on three of four ticks. Expect the
  world-tick number to rise in this phase before the later phases bring it down; that is not a
  regression. The from-scratch retrain absorbs the dynamics change; record before/after with `forge bench`.
- Keep a worker path for grid preload (`MapUpdater::schedule_map_preload` is what
  `PreloadAllNonInstancedMapGrids` uses); Phase 3's full preload depends on it.
- Core pinning: confirm which cores carry the 96 MB V-cache with `lscpu -e` and the cache sizes at
  implementation time rather than hardcoding 0-7.

**Contained fixes**, each measurable on its own:

1. `Map::_updateObjects` insert on every dirty field (`Object.cpp:529`) becomes a no-op; the forge only
   clears masks. Gate `Object::BuildValuesUpdateBlockForPlayer` (`Object.cpp:256`) and the
   `AddOutOfRangeGUID` leak in `VisibleNotifier::SendToSelf` (`GridNotifiers.cpp:72`).
2. Per-map player index on `Map` (`m_mapRefMgr` lookup) used by `Aura::UpdateOwner→GetCaster()` and every
   `ObjectAccessor::GetPlayer(Map const*, guid)` / `GetUnit` so map threads stop taking the global
   `HashMapHolder<Player>` shared_mutex (`ObjectAccessor.cpp:85-94, 244-251`).
3. Delete `Unit::_valuesUpdateCache` (`Unit.h:2257`, cleared every tick at `Unit.cpp:637`).
4. `UpdateMask` byte-per-field → real bitset (`UpdateMask.h`); `AppendToPacket` is dead on forge.
5. `Unit::GetTotalAuraModifier` family: template on the predicate and hoist the per-call
   `std::map<SpellGroup,int32>` (`Unit.cpp:6217-6240`) into a small stack array.
6. `std::list` → `std::vector` / `boost::container::small_vector` for `Spell::m_UniqueTargetInfo`,
   `m_UniqueGOTargetInfo` (`Spell.h:707,717`) and the grid searcher result lists.
7. `Unit::m_modAuras[317]` → sparse (one vector of `(type, AuraEffect*)` with per-type offsets):
   7.6 KB → ~0.3 KB per unit, and the largest single RSS win for a full world.
8. `resetMarkedCells` memset → per-cell generation stamp (`Map.cpp:479`, `Map.h:637`).
9. Restore the `Player::RegenerateAll` early-return guard (`Player.cpp:1803`).
10. Remove the `dynamic_cast<UpdatableMapObject*>` on the add/remove path (`Map.cpp:588-610`).

## Phase 2.5: fold mod-animus-forge into the core

**Why.** With no rebase to protect, the module boundary only costs. The module is the sole
registrant of every unit script hook (its `UnitScript` passes no hook list, so the core enables all
of them and walks a vector plus a virtual no-op per unit per tick and per melee swing); every damage
event pays two script dispatches and a thread-local hand-off because `ScriptMgr::DealDamage` carries
no spell; heals are correlated by an argument-order guess the code calls fragile; `CoreHooks` wraps
three one-line core calls; `PoolRegistry` fans out to the one pool owner; the module locates itself
from `__FILE__`; and Phase 3 needs this code on map threads with core access. The module is a
separate git repo ignored by the core (`.gitignore:8`), so this is an import, not a move.

**Where it goes**

- `src/server/forge/` (new static library `forge`, built like `game`: `CollectSourceFiles` +
  recursive `CollectIncludeDirectories`, so the module's flat includes keep working) takes
  `modules/mod-animus-forge/src/*` minus what is deleted below. `src/server/game/Forge/` (the core
  seams: `Forge.{h,cpp}`) stays in `game`; `forge` links `game` and the worldserver links `forge`.
- `apps/forge/python/` takes `python/` (learner, dashboard, tests, `pyproject.toml`); `apps/forge/tools/`
  takes `tools/`; `apps/forge/models/` takes `models/`; `docs/forge/` takes `docs/manual/`.
- `conf/mod_animus_forge.conf.dist` becomes a `FORGE` section appended to
  `src/server/apps/worldserver/worldserver.conf.dist`. **Keys keep the `AnimusForge.` prefix** (339
  keys; the live conf and the dashboard depend on the names). The module `.conf` install code and
  `LoadModulesConfigs` are no longer needed for it; `ForgeMain` reads everything from `worldserver.conf`.
- History stays in the archived module repo; the import is a plain copy in one commit
  ("imported from Moloch17/animus-forge at <sha>").

**What is deleted or replaced on the way in**

- `Hooks/AnimusForgeScripts.cpp`, `Hooks/AnimusLibScripts.cpp`, `animus_forge_loader.cpp`,
  `Core/CoreHooks.*`, `Env/PoolRegistry.*`, `mod-animus-forge.cmake`, the `thread_local
  PendingSpellDamage`/`PendingHeal` structs and `Animus::PendingSummonLevel`.
- `World::Update` calls the host directly (`sAnimusForge->OnStartup/OnUpdate/OnShutdown` from
  `ForgeMain`/`World.cpp`) instead of `WorldScript`.
- Damage, heal and cast recording become direct calls from the core sites with the spell in hand:
  `Unit::DealDamage` (attacker, victim, amount, type, `SpellInfo const*`), `Unit::HealBySpell` /
  periodic heal ticks (periodic flag known at the site), `Spell::cast` / `Spell::cancel`. The summon
  level becomes a parameter of the summon helper the curriculum calls, not a global read by
  `AllCreatureScript`.
- The `forge` console command table moves to `src/server/scripts/Commands/cs_forge.cpp` (commands stay
  script-registered in the core, as every command is).
- Rename the `AnimusForge::Forge` class (it collides with `namespace Forge` once both are core code):
  `AnimusForge::Host`, macro `sAnimusForge` unchanged.
- `ForgeConfig.cpp`'s `ModuleRoot()` becomes a CMake-configured `FORGE_PYTHON_DIR` (the source tree's
  `apps/forge/python`, bind-mounted at `/azerothcore` in the container) overridable by
  `AnimusForge.Learner.WorkDir`; `LearnerProcess.cpp`'s error text and `dashboard.py`'s `parents[4]`
  anchor follow.
- `apps/docker/animus-venv.sh`, `apps/docker/forge-worldserver.sh`, `docker-compose.yml` (pytest
  recipe, comments), `apps/docker/Dockerfile.dev-server`: every `modules/mod-animus-forge` path becomes
  `apps/forge`. The `AC_MODULES_LIST` / `CONFIG_FILE_LIST` plumbing stays for any other module.
- Python tests re-anchored: `parents[2]/src/...` → `<repo>/src/server/forge/...`,
  `parents[2]/conf/...` → the worldserver conf template, `docs/manual` → `docs/forge`. The syntax
  check needs nothing (`var/syntax_check_core.py` is path-substring driven).
- `mod-animus.cmake:28-36` (the mutual-exclusion check against mod-animus-forge) becomes an
  unconditional refusal to build on the forge core: its bundled curriculum would collide with core
  symbols. It stays a stock-realm module and is never part of a forge build.

**Then**, in the same phase, the two cleanups the fold makes trivial: the unit-hook subscription
problem disappears with the `UnitScript`; and the deferred module-side items from Phase 0/1 land as
core work: the reset timer split (Create / Configure / Place / Destroy), `WarmCaches()` before the seal
(then `ForgeSealStrict = true` and MySQL stopped after startup), the playtest tick-diff tolerance in
the decision loop, and `MapMgr::GetUpdateTiming()` in `forge status`.

**Verification.** Configure and a full syntax sweep of `src/server/forge` (the 88 module TUs already
in the compile database) plus every core site that gained a direct call; `pytest` from
`apps/forge/python` inside the container; `forge status` prints the per-phase map timing; a
learner-driven run reaches its first reset without a "Synchronous query on sealed" line once
`WarmCaches()` is in; grep shows no `mod-animus-forge` path outside the archived repo's README.

## Phase 3: module work onto map threads; replica continents

1. **Observe/reward per map.** Done (2026-09-25, syntax-checked, unmeasured). The script hook the plan
   named is gone with the module fold, so the seam is `MapUpdater::RunMapTick` -- the one definition of a
   map's whole tick, which the workers and the two inline paths (no threads configured) all go through.
   It calls `Forge::OnMapPrologue` before `Map::Update` and `Forge::OnMapEpilogue` after
   `Map::DelayedUpdate`.

   A decision is now three parts on `EnvPool`. `BeginDecision` opens it on the world thread from
   `Forge::OnWorldPrologue`, called from `World::Update` before `MapMgr::Update`: the episode clock is
   advanced there (the terminal check after the maps reads it) and the tick-to-decision count moved there
   with it, so both are settled before the first map task starts. `ApplyActionsForMap` and `ObserveMap`
   run on the thread updating that map, over `_mapEnvs` (envs keyed by map id and instance id, maintained
   by `IndexEnv` on the world thread like `_agents`), writing only their own envs' slices of `Obs`,
   `State`, `Mask` and `Rewards`. `FinishCollect` closes the decision on the world thread and keeps what
   must stay serial: an ended episode's info, its report, and its reset (item 3 moves resets).

   Per-decision timings are per-env slots summed by `FinishCollect`, so no two map threads write one
   counter; the apply is an atomic accumulator, since the maps of the tick after a decision run it.
   Observe, reward and apply are now thread time inside the world figure rather than time on top of it,
   and `forge status` says so instead of implying they add to the sim number.

   Audited for the move: the curriculum keeps per-env state in vectors indexed by `env.Index`
   (`StageScenario::_data`, each encounter's own `_envs`), its shared members are read-only tables built
   at setup, and `DifficultyLadder` already had a mutex for finishing on map threads. An env whose map no
   task ticked is scored by `FinishCollect` itself with one error line, so a scheduler gap costs a stall,
   not a stale transition.

   Still to do here: `RecordDamage/RecordHeal/RecordCast` index `_agents` by a `Player`-stored agent slot
   instead of an `unordered_map` find. Instance stages parallelise with this; continent stages need item 2,
   since every env on one base map is one task.
2. **Replica continents.** Done (2026-09-25, syntax-checked, unmeasured). Less was needed than this item
   assumed: a replica is `new Map(id, GenerateInstanceId(), REGULAR_DIFFICULTY, base)` and nothing else.
   `GridTerrainLoader::LoadMap` already gives any map with an instance id its parent's terrain, and
   `MapCollisionData` already shares the parent's collision tree and navmesh while keeping a nav query per
   map object, which is what makes concurrent pathfinding safe. So `Map::Instanceable`/`ToMapInstanced` are
   untouched, continents stay non-instanceable, and no `InstanceMap` machinery is involved.

   `MapMgr::CreateContinentReplica(mapId, index)` creates them on first use under the manager's lock, index
   0 being the base map; `FindMap` gains one branch for a non-instanceable map with an instance id;
   `MapMgr::Update` schedules replicas like any other map; both `DoForAllMaps` forms reach them, so world
   spawns and game events are not confined to the base; `UnloadAll` drops them before the base whose terrain
   they hold. The base's grids are loaded before any replica, since a replica's grid asks the base for
   terrain that must already exist.

   Envs are dealt out in **contiguous blocks**, not `env.Index % N`: a map has 31 phases to give and
   consecutive indexes in a block of at most 31 have distinct remainders, so `EnvPhase` is unchanged.
   `AnimusForge.ContinentReplicas` (`StageSettings::ContinentReplicas`) sizes it, 0 meaning the fewest the
   phase cap allows, which leaves a pool of 31 envs or fewer on one map exactly as before and removes the
   31-env cap above it. `Player::TeleportTo` needed nothing: every curriculum teleport is within the map,
   and that branch keeps the map object, replica included.

   Costs, after the follow-up that stopped replicas loading whole continents: a replica shares terrain, the
   collision tree and the navmesh with the base, and loads grids only where its envs stand, so it costs the
   creatures and gameobjects of those grids. `Map::OnCreateMap` loads all grids for a map with an instance id,
   which is right for a dungeon and wrong for a continent, so it now asks `Instanceable()` too; the base keeps
   `PreloadAllNonInstancedMapGrids` for anyone who wants the whole world live. `MapGridManager::CreateGrid`
   already creates the parent's grid first under the parent's lock, so a replica's terrain is always there and
   two replicas asking at once is safe; the base ends up holding terrain-only grids for the union of what its
   replicas touch, which is the one copy of the terrain that would exist anyway.

   The plan's ~450 MB per replica was the whole-continent load, not the floor. Zone-keyed world state (outdoor
   PvP, weather, world states) is one set per map id and is shared across a continent's replicas; nothing the
   sim does touches it today.
3. **Reset on the map thread.** `ObjectGuidGeneratorBase::_nextGuid` → `std::atomic` with `fetch_add`
   (`ObjectGuid.h:298-315`). `HashMapHolder::Insert/Remove` and `CharacterCache::AddCharacterCacheEntry`
   become per-thread batches applied by the world thread at the barrier. Then `BotFactory::Create` and
   `SeatCharacter::Configure` run inside the map task; `AddToMap` is deferred to the barrier.
4. **Warm instance pool.** Instance stages keep instances alive across episodes and reset creatures in
   place instead of `DestroyInstance` + `CreateInstance` (grid load + spawn per episode is the suspected
   bulk of the 6.4 ms; the Phase 0 timers decide).
5. **Character templates.** One prebuilt `Player` per (class, spec, level band, kit): clone by copying
   spell/talent sets, non-guid stat field ranges, and gear as an item-id list with fresh guids, then one
   `UpdateAllStats()`. Never memcpy whole `m_uint32Values` (guid-bearing fields, aura objects).

## Phase 4: learner exchange

1. **Shared-memory exchange.** Replace the Unix-socket copy in `Bridge/LockstepServer` and
   `python/animus/protocol.py` with a shared-memory ring (two obs/action buffers + eventfd); the learner
   maps it with `numpy.frombuffer`. Expected 10.1 → ~4 ms per decision.
2. **Half-batch double buffering.** Split envs into halves A/B; tick A while inference runs B.
   `StepHeader` gains `EnvBegin/EnvCount`; the MAPPO buffer `[T, E, A, …]` takes two half writes per t;
   GRU memory is per env. The exchange moves to its own thread so the scheduler barrier never waits on
   it. Build on the existing learner-internal `overlap_updates`, which overlaps the PPO update with the
   next rollout and stays as is.
3. **Learner on the GPU.** Enable the AMD compose block (`/dev/kfd`, `/dev/dri`, render/video gids),
   ROCm torch wheel via `ANIMUS_TORCH_INDEX_URL`; `train_device` already resolves to it. Move rollout
   inference to the device as well (`rollout_device`), batched over all envs; Phase 6 then feeds it
   device-resident observations so the batch never crosses PCIe.

## Phase 5: hot-state mirror (CPU) and the device runtime

**Hot state.** `src/server/game/Forge/HotState.{h,cpp}` per `Map`: dense index assigned in `AddToMap`,
freed in `RemoveFromMap`, stored on `WorldObject::_hotIndex`. 64-byte-aligned SoA columns: `x,y,z,o`,
`guid`, `typeId`, `faction`, `phaseMask`, `health/maxHealth/power/maxPower`, flags (alive, combat,
player, bot, pet, hostile), `visRadius`, spline velocity, `gridIndex`, per-row dirty generation.
Write-through from `Position::Relocate`, `Unit::SetHealth/SetPower`, the flag/faction setters in
`Object::SetUInt32Value`, `SetPhaseMask`; double-buffered `cur/prev` swapped at tick end. This is the
host-side twin of the device buffers and the source of the per-tick dirty delta.

**Kernel interface.** `src/server/game/Forge/HotKernels.h`: `RangeQuery`, `VisibilityPairs`,
`TerrainHeight`, `LiquidStatus`, `LineOfSight`, `PathQuery`, `SplineAdvance`, `RegenTick`, span in /
caller buffer out, no allocation. Two implementations behind it: `HotKernelsCpu.cpp` (AVX-512 via
`#pragma omp simd` plus a small `simd.h` of `_mm512_*` for the distance test and bilinear gather) and
`HotKernelsHip.cpp` (Phase 6). The CPU twin is the correctness reference and the path for the CPU
exceptions; it is not a throughput path once the state is device-resident.

**Device runtime.** `src/server/game/Forge/Gpu/` with HIP (ROCm; gfx1100). Container: ROCm dev base
image or `hip-runtime-amd` + `hipcc` added to `apps/docker/Dockerfile.dev-server`; enable the AMD compose
block (`/dev/kfd`, `/dev/dri`, render/video gids). CMake option only for the toolchain path, not for
behaviour. Vulkan compute is the fallback only if ROCm userspace for gfx1100 fails in the container.

## Phase 6: map data and unit state on the device

1. **Static map upload at load** (`Gpu/DeviceWorld.{h,cpp}`), once per map id, read-only after:
   - Terrain: `GridTerrainData` V9/V8 heights, height format, liquid heights/flags, area ids per grid
     into one device array per continent with a grid offset table (~350 MB total, u16 where the file
     is u16).
   - Collision: `StaticMapTree` BIH nodes + `WorldModel`/`GroupModel` triangles flattened to
     node/triangle buffers, plus `ModelInstance` transforms (~600 MB).
   - Navmesh: `dtMeshTile` vertices, polys, links, detail meshes flattened with a tile offset table
     (~1.3 GB). The `dtNavMesh` stays on the host for the CPU twin.
2. **Unit state on the device** (`Gpu/DeviceUnits.{h,cpp}`): the hot-state columns mirrored to device
   buffers sized for all maps; per tick the host uploads only dirty rows (index list + column values,
   tens of KB at thousands of bots) and downloads the rows the device changed.
3. **Device tick pipeline** (`Gpu/Pipeline.cpp`), launched once per world tick as a stream of kernels
   with no host sync in between:
   1. `SplineAdvance`: integrate movement for every moving unit.
   2. `TerrainHeight` + `LiquidStatus`: ground/water snap for the moved rows.
   3. `LineOfSight`: BVH traversal for every (unit, target) pair that needs it this tick.
   4. `VisibilityPairs` and `RangeQuery`: uniform cell hash built on the device (counting sort), then
      visibility sets, aggro candidates and threat-proximity per unit.
   5. `PathQuery`: nearest-poly, A* on the flattened navmesh with one workgroup per query and a bounded
      open set, funnel to a polyline; results land in per-unit path buffers. Per-tile poly-to-poly
      distance tables precomputed on the device at load shorten long paths (hierarchical A*).
   5b. **Movement generators as a table** (`MovementIntents`: unit, mode chase/follow/flee/point/random/
      home, target row or position, desired range, repath timer): a kernel decides per unit whether to
      repath, walk the current path, stop at range, or return home, and writes the spline for step 1 of
      the next tick. `MotionMaster` on the CPU becomes a thin projection for the CPU exceptions; the
      chase/follow repath that today runs through the single per-map `dtNavMeshQuery` is gone from the CPU.
   6. `RegenTick` and simple periodic ticks for units flagged as unmodified by auras.
   7. **Observation encoding**: the module's `SeatEncoder` layout rewritten as a kernel over the
      device columns, writing `Obs[E*A*O]` and `Mask` directly into a device tensor.
4. **Inference on the same card.** The learner maps the sim's device tensors through HIP IPC memory
   handles (`hipIpcGetMemHandle`) so `Obs` never crosses PCIe; rollout inference runs on the device and
   writes `Actions` back into a shared device buffer; the MAPPO rollout buffer lives on the device. The
   Phase 4 shared-memory ring becomes the control channel only (headers, episode info). This replaces
   the learner round trip entirely.
5. **Host consumers** switch from the grid walks to the device results in value order:
   `StageScenario::ObserveSeat` (deleted in favour of the kernel) → `VisibleNotifier::SendToSelf` /
   `Player::UpdateVisibilityOf` → `AIRelocationNotifier` → radius searchers used by spells →
   `PathGenerator` (reads the device path buffer; CPU Detour only for the twin) → `Unit::UpdateSplineMovement`.
6. **Delta back to the CPU**: positions, LOS results, visible sets, paths, regen values are applied to
   the `Unit` objects by the owning map thread at the start of the next tick, before spells and AI run.
   Discrete CPU events (damage, aura state, death, teleport, summon) mark rows dirty for the next upload.

Dispatch rule: **the device is the default for every data-parallel step.** A kernel whose inputs already
live on the device runs there regardless of batch size, because the alternative is a PCIe copy in each
direction plus a host loop; the launches are queued on one stream with no host sync between them, so
their fixed cost overlaps. The CPU twin runs only in the differential harness and on the CPU-exception
path. Anything host-side that vectorises (mask clears, the exception path's own loops) stays AVX-512
because its data is not on the device; the moment such data moves to a table, its kernel moves with it.
Kill criterion per kernel is correctness against the twin, not a per-kernel speed race.

### Multi-GPU (2-3 cards)

The partition is **map ownership**. Units on different maps never interact except through teleports,
so each device owns a set of maps (continent replicas, instances, battlegrounds) and runs the whole
pipeline for them on its own stream with its own unit tables. Nothing crosses devices per tick.

- `Gpu/DeviceSet`: enumerates HIP devices (`Forge.Gpu.Devices` lists which to use; default all).
  `MapMgr` assigns a device to each map/replica at creation, balancing by expected load (replicas of
  the same continent spread round-robin) and records it on the `Map`; the host scheduler groups map
  tasks by device so one host thread drives one device's stream.
- Static data: a full set (terrain + BVH + navmesh, ~2.3 GB) is uploaded to every device; simpler than
  per-device subsets and well within memory next to the tables and the learner.
- Teleport/transfer between maps on different devices = row migration through the host delta (the
  same path an instance entry takes today); rare, not per tick.
- Learner: one policy replica per device; each device runs inference on its own maps' observations and
  fills its shard of the MAPPO buffer `[T, E_dev, A, …]` in its own memory. Training is DDP across the
  cards (gradient all-reduce over the small MAPPO nets) or one trainer card gathering the shards,
  whichever the update-time measurement favours; weights broadcast after each update. `overlap_updates`
  applies per shard.
- PCIe traffic per tick is the delta and the action buffer, tens of KB per device, so x8/x4 lane splits
  on a consumer board do not matter. No peer-to-peer required.
- Playtest mode: the human's map is on some device like any other.

## Phase 7: the combat core as device tables

The end state the user asked for: everything that can be represented as data is a device table, and the
tick over those tables is a kernel sequence. AzerothCore's combat rules are already table data underneath
(DBC spell effects, aura modifier types, class/level stats, faction templates, threat, cooldowns); only
the C++ object model makes them look like pointer-chasing. `src/server/game/Forge/Gpu/Combat/`.

**Static tables uploaded once at load** (from the same DBC/DB structures the CPU core loads):
`SpellInfo` (effects, aura types, base points, ranges, costs, cast/cooldown/GCD, school, mechanic,
attributes, stack rules, proc flags), `SpellRadius/Range/Duration/CastTimes`, `FactionTemplate`
(hostility masks), `player_classlevelstats`/`player_levelstats`/`creature_classlevelstats`, item stat
sums per template, talent→spell map, `SpellGroup` stack rules, `smart_scripts` (generic creature rules).

**Dynamic row tables** (device-resident, host mirrors only for the CPU exceptions):

| Table | Row | Kernel per tick |
|---|---|---|
| Units | the Phase 6 SoA columns + stats, resistances, ratings, attack timers, GCD, cast state | stats formula, timers, regen |
| Auras | target, caster, spell, effect index, aura type, amount, remaining, period, stacks, charges, absorb left | periodic ticks, expiry + compaction, segmented reduction → `UnitMods[units × auraTypes]` |
| CastRequests | caster, spell, target/position, queued at | validate (cost, cooldown, range, facing, LOS, state) → rolls (hit/crit/resist/block, device RNG) → outcome |
| Outcomes | damage/heal/absorb/miss per (caster, target, spell) | apply to health/power, append Auras, append Threat, proc triggers, reward hooks |
| Threat | creature, target, amount | decay/modifiers, segmented argmax → current target |
| Cooldowns | unit, spell/category, remaining | decrement |
| Movement intents | unit, mode (chase/follow/flee/point/random/home), target, range, repath timer | Phase 6 path/spline kernels |
| CreatureAI state | state (idle/combat/evade/dead), combat pulse, boundary/leash, home position, current target, respawn timer, SmartAI phase, per-event cooldowns and counters | rule kernel over `smart_scripts`: evaluates event conditions (timers, health pct, range, aggro, phase) and appends CastRequests / MovementIntents / state changes |

`UnitMods` replaces `GetTotalAuraModifier*` (about twenty list scans per swing today) with a table
lookup; `SpellDamageBonusDone/Taken`, `MeleeDamageBonusDone`, `CalcAbsorbResist`, `MagicSpellHitResult`
become formulas over `UnitMods` and the spell table.

**CPU exceptions.** Spells with hand-written behaviour (`spell_*` scripts, `Spell::EffectXxx` special
cases, boss scripts) are flagged in the spell table; their casts take the CPU path through the existing
`Spell`/`Aura` objects and their results enter the tables as dirty rows. Inventory, quests, professions,
mail, auction, guilds, loot rolls, and the life systems stay object-based on the CPU: rare events, not
per-tick work. Generic creature behaviour runs as a rule kernel over the `smart_scripts` table reading and writing
the `CreatureAI state` table, so a creature's whole decision (aggro, target, cast, move, evade, respawn)
happens on the device; creatures whose AI is C++ (bosses, special scripts) are CPU exceptions whose
state row is mirrored from the object. Idle-world work (random wander, respawn countdowns, regen out of
combat) is the same kernels over the same rows, which is what keeps 150k spawns cheap.

**Oracle.** The existing core is the reference. A differential harness (`src/test/forge/`) runs the same
cast requests through both engines on seeded RNG and compares outcomes, aura tables and health; any
divergence is a device-engine bug. Rollout is table-first only when the curriculum's action catalog
reports its coverage: `forge status` gains a line with the share of casts resolved on the device versus
the CPU exceptions, per stage, so the exception list is worked down by measured value.

**Order inside the phase**: Auras + `UnitMods` (removes the aura scans everywhere) → Cooldowns/GCD →
Threat → CastRequests/Outcomes for the generic effect set (direct damage/heal, periodic, stat and
speed mods, absorbs, stuns/roots/silences) → stats formula → CreatureAI state table + SmartAI rule
kernel (with the Phase 6 movement intents, this moves creature decisions off the CPU entirely).

## Phase 8: intra-map parallelism for the CPU exceptions (grid-block checkerboard)

Once the device owns the continuous state and the generic combat tables, what remains per map on the
CPU is the exception list: scripted spells, C++ creature AI, and the life systems. When one replica with
thousands of bots puts that on the critical path:

- Block = one grid (533 yd); colour `(gx & 1) | ((gy & 1) << 1)`; four barriers per tick; blocks of one
  colour run in parallel (every routine interaction radius is ≤ 400 yd + searcher compensation).
- Invariants: far reads go through the `prev` hot-state snapshot; far writes (`DealDamage`, `CastSpell`,
  `AddAura`, `SetTarget`, `Kill`, `Teleport`, `SummonCreature`) assert owner-thread or enqueue to a
  per-thread deferred ring applied at the barrier. Player relocation joins `_creaturesToMove` as
  `_playersToMove`. `AddToMap`/`RemoveFromMap` inside a block task route through the delayed lists.
- `_updatableObjectList` becomes per-grid sub-vectors. The CPU-twin `dtNavMeshQuery` becomes one per
  worker per navmesh (`MMapData::GetNavMeshQuery(workerIndex)`, `MapCollisionData.cpp:177-180`). RNG is
  already thread-local.
- `FORGE_THREAD_ASSERTS` build run through every stage with `forge bench` finds cross-block writes
  empirically; the assert list is the audit.
- Module `StepStats` become per-worker accumulators merged at the barrier.

## Verification

- Baseline and after every phase: `forge status` per-decision line (world / reset / observe / apply /
  learner) at the same stage, env count and thread count; RSS before and after a `forge bench` sweep.
- Phase 1: thread listing has no `DatabaseWorker`; no MySQL sockets; tripwire silent across a
  learner-driven pass over every stage (a random policy exercises every reset and action path).
- Playtest mode: with `Forge.Playtest = 1` a real client logs in through the authserver, sees the world
  update (movement, combat logs, auras, bots moving), Warden runs its checks, and the player's character
  saves; with it off the listener is absent and the per-decision timing line matches sim mode.
- Phase 2: `MapUpdate.Threads` sweep via `forge bench` shows world time falling with threads on instance
  stages; a short learner-driven run (`forge start <stage>` for a few hundred decisions) on a dungeon, a
  battleground and a continent stage confirms the tick, resets and episode completion still work.
- Phase 3: a continent stage with 128+ envs shows per-map task times in the timing line and the world
  thread's own share near the exchange only.
- Phase 4: learner ms per decision, then env steps/s with and without a learner (baseline 20-27k / 5-10k).
- Phase 5-6: per-kernel micro-benchmarks (CPU scalar vs AVX-512 vs HIP at batch sizes 1k-1M) checked in
  under `src/test/`; device results compared against the CPU twin on the same inputs (height, LOS,
  visibility sets, paths within tolerance); device memory reported at load (terrain, BVH, navmesh,
  unit buffers); a continent stage at thousands of bots shows the timing line's world tick dominated
  by the CPU logic phase with observe and learner exchange near zero.
- Multi-GPU: with two or three cards, `forge status` reports per-device tick time and env count; env
  steps/s scales near-linearly with cards on a continent stage with replicas spread across them; a
  cross-device teleport in playtest mode lands the player correctly.
- Phase 7: the differential harness passes on the generic effect set; the coverage line shows the
  device share of casts per stage rising as exceptions are worked down; the aura-scan and cast paths
  disappear from the CPU profile.
- Phase 8: `FORGE_THREAD_ASSERTS` silent across every stage; world tick falls with threads on one
  replica.
- Syntax check per batch; `python apps/codestyle/codestyle-cpp.py`; unit tests in `src/test/` for the
  bitset mask, sparse aura store, atomic guid generator and hot-state sync.
- Note: memory `feedback-no-smoke-until-stages-added` currently forbids `forge run/start`; the user
  decides when the smoke runs begin.

## Resume here (2026-09-25, second session)

State: `forge` has everything committed, unpushed (the user has not asked for a push);
`worktree-forge-parallel-core` is behind at 363cb745c. The worldserver is built with the placement change and
running idle. The user's standing priorities: multithreading as fast and efficient as possible, and as much
as possible offloaded to the GPU; judge by end-to-end env steps/s with the learner attached.

Open items, most useful first:

1. **The user's decision on overlap_updates** (1.65x on the real-sim bench for one update of staleness). If on, set it
   in configs/stage8_duel.yaml (the curriculum extends it) and re-run `forge bench` with the learner.
2. **The rollout side is now the wall when overlapped**: ~9.8 ms per decision = sim ~5-6 ms + learner ~4 ms
   (CPU inference for 128 envs, buffer writes, the socket copy). Phase 4's shared-memory ring and half-batch
   double buffering attack the learner's 4 ms; profile `bench_learner` first to split inference from transport.
3. **Settings to apply once the user agrees** (performance only, no training effect): MapUpdate.Threads 16 -> 7
   and AnimusForge.ContinentReplicas 0 -> 8 (fourth measurement). Envs 192 would change the learner's batch:
   the user's call.
4. The update's remaining cost is MIOpen's per-timestep launches (~880 ms CPU of 1.18 s). Only fewer, larger
   sequential sweeps (fewer minibatches) change that: a training hyperparameter, the user's call.
5. The learner is pinned with sched_setaffinity(pid) right after posix_spawn: that pins the main thread only, and
   torch's threads inherit it because Python starts them much later. If that ever races, prefix the argv with
   `taskset -c <list>` instead.
6. Carried over from the first session: the folded-module config fix depends on an untracked directory
   (`modules/<module>/conf`); the abort handler segfaults before its backtrace; the live worldserver.conf
   predates the fold; `AnimusForge.Bench.Policy` is left at "random".
