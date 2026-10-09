# C++ runtime: the plan runner, the env pool, the bridge and the console

Purpose and scope. This is the entry point for the C++ code that is not a scenario, a movement controller or a camera:
the module root `AnimusForge::Forge` (plan runner, tick and decision bookkeeping, benchmark), the lock-step bridge to the
learner, the cluster link, the env pool, the session-less bots, the console and its report, the learner launcher, the
GPU device layer, the worldserver entry point and the CMake wiring. Everything is under `src/server/game/Animus/`
except where the map table says otherwise. Movement, camera and scenario code are documented in
[cpp-movement.md](cpp-movement.md), [cpp-vision.md](cpp-vision.md) and [cpp-stagescenario.md](cpp-stagescenario.md);
the whole picture is in [00-architecture.md](00-architecture.md); what the fork changed in the AzerothCore core is in
[01-forge-core-delta.md](01-forge-core-delta.md). All paths below are relative to the repository root; `A/` abbreviates
`src/server/game/Animus/`.

This document set (every line number is for commit `bd32b9dc8`):

| File | Content |
|---|---|
| `cpp-runtime.md` (this) | map table, threading and ownership, the `Forge` class, the plan runner, the benchmark |
| [cpp-runtime-bridge.md](cpp-runtime-bridge.md) | wire protocol, `LockstepServer`, the decision message loop, device buffers, `ClusterLink`, the fingerprint |
| [cpp-runtime-env.md](cpp-runtime-env.md) | `EnvPool`, `Env`, hooks, `ResetDefer`, reset timing, kinematics, bots |
| [cpp-runtime-console.md](cpp-runtime-console.md) | every `forge` command, `Progress`, `TextTable`, which `progress.json` keys the status reads |
| [cpp-runtime-config.md](cpp-runtime-config.md) | every config key read by the runtime, with its default |
| [cpp-runtime-process-gpu.md](cpp-runtime-process-gpu.md) | `ForgeMain`, `ForgeCore`, learner launcher, CMake hash and device library, `Gpu/` |

## Map table

Lines are `wc -l` at `bd32b9dc8`.

| Path | Lines | Role |
|---|---|---|
| `A/AnimusForge.h` | 584 | declares `AnimusForge::Forge`, the module root: states, plan, bench and cluster fields |
| `A/AnimusForge.cpp` | 3087 | the decision loop, plan runner, cluster polling, bench, status snapshot, STEP/ACT exchange, evaluation videos |
| `A/ForgeCommands.cpp` | 1304 | the `Forge::Command*` bodies behind the console (status, start, resume, bench, export, clean ...) |
| `A/ForgeConfig.h` | 318 | `ForgeConfig`: every setting the runtime reads, derived profiles (fast, bench) |
| `A/ForgeConfig.cpp` | 763 | `ForgeConfig::Load`, GPU detection, bench grids, profiles, path resolution |
| `A/Bot/BotAccounts.h` | 58 | account-id ranges of the in-memory bots |
| `A/Bot/BotFactory.h` | 98 | declares creating, placing, teleporting, destroying a session-less bot |
| `A/Bot/BotFactory.cpp` | 307 | implements the above on public core APIs plus the fork's `SetSimSession`/`EnableMovementOrders` |
| `A/Bot/BotSlot.h` | 90 | one seat's pair of alternating bots/sessions/GUIDs |
| `A/Bot/BotSlot.cpp` | 123 | `BotSlot` implementation |
| `A/Bridge/Protocol.h` | 401 | wire protocol v25 structs, `CutAct`, `BadLookRow` |
| `A/Bridge/LockstepServer.h` | 95 | blocking POSIX socket server (Unix or TCP), one client per learner rank |
| `A/Bridge/LockstepServer.cpp` | 396 | listen/accept/HELLO, `writev` send, polled receive |
| `A/Bridge/ClusterLink.h` | 158 | cluster control channel, host and worker roles |
| `A/Bridge/ClusterLink.cpp` | 463 | line protocol over non-blocking TCP, fingerprint check |
| `A/Console/Progress.h` | 290 | `ProgressFile` (reader), `SimSnapshot`, `ProgressMonitor` |
| `A/Console/Progress.cpp` | 835 | builds the `forge status` / periodic report tables |
| `A/Console/TextTable.h` | 100 | `LineSink`, `TextTable`, `Format::*` |
| `A/Console/TextTable.cpp` | 188 | their implementation |
| `A/Env/Env.h` | 170 | `Env`, `AgentStats` |
| `A/Env/Env.cpp` | 64 | `Env::Find*` |
| `A/Env/EnvPool.h` | 394 | `EnvPool`: the flat SoA buffers and the decision phases |
| `A/Env/EnvPool.cpp` | 942 | its implementation, the damage/heal/cast hooks' bodies, seeding |
| `A/Env/AnimusHooks.h` | 53 | free functions the core calls on combat events |
| `A/Env/AnimusHooks.cpp` | 71 | forward to the active pool |
| `A/Env/ResetDefer.h` | 58 | queue for the world-thread half of a map-thread reset |
| `A/Env/ResetDefer.cpp` | 66 | its implementation |
| `A/Env/ResetTiming.h` | 219 | per-reset timers, `ResetSamples` ring, `Stall` verdict (header-only) |
| `A/Env/Kinematics.h` | 102 | the 10-float body sample a STEP carries per agent (header-only) |
| `A/Json/BoostJson.cpp` | 30 | the one translation unit that includes `boost/json/src.hpp` |
| `A/Learner/ChildProcess.h` | 83 | generic child process (learner, export) |
| `A/Learner/ChildProcess.cpp` | 171 | `posix_spawn`, reap, stop escalation |
| `A/Learner/LearnerProcess.h` | 82 | the learner as one or more rank processes |
| `A/Learner/LearnerProcess.cpp` | 328 | command line, devices, CPU pinning |
| `A/Gpu/DeviceApi.h` | 68 | the C table between worldserver and `libforge-gpu.so` |
| `A/Gpu/GpuRuntime.h` | 51 | loader API |
| `A/Gpu/GpuRuntime.cpp` | 131 | `dlopen` of torch's HIP runtime and of the device library |
| `A/Gpu/Device/DeviceRuntime.h` | 60 | HIP/CUDA name mapping inside the library |
| `A/Gpu/Device/Runtime.hip` | 127 | allocation, IPC export, copies, the API table |
| `src/server/apps/worldserver/ForgeMain.cpp` | 520 | `main()` of the fork: startup, DB seal, update loops, shutdown |
| `src/server/scripts/Commands/cs_forge.cpp` | 986 | the `forge` command table and the handlers that are not `Forge::Command*` |
| `src/server/game/CMakeLists.txt` | 146 | Animus source hash, `FORGE_PYTHON_DIR`, `libforge-gpu.so` |
| `src/server/game/Forge/Forge.h`, `Forge.cpp` | 51 + 60 | `ForgeCore`: `HasClients()` (false), `SetTickMs/TickMs` (owner: 01-forge-core-delta.md, used here) |
| `src/common/Threading/CpuPlacement.h`, `.cpp` | 62 + 278 | CPU ordering and pinning used by `LearnerProcess` and `ForgeConfig` (owner: 01-forge-core-delta.md) |
| `src/common/Utilities/RandomSeed.h` | 28 | `rand_seed`, used by `EnvPool::ResetEnv` (owner: 01-forge-core-delta.md) |
| `src/server/database/Database/DatabaseWorkerPool.h` | 260 | `Seal(strict)` / `WarnAboutSyncQueries`, used by `ForgeMain` (owner: 01-forge-core-delta.md) |

Other Animus code under `src/common` is only `Config::LoadAdditionalFile` (`src/common/Configuration/Config.h:74`).
`A/` also holds `Movement/`, `Vision/`, `Scenario/` (other documents). Hooks into the core that call this runtime:
`src/server/game/World/World.cpp:1233` (`OnWorldPrologue`), `:1258` (`ProcessCliCommands`), `:1261` (`OnUpdate`);
`src/server/game/Maps/MapUpdater.cpp:219,222` (`OnMapPrologue/Epilogue`); `src/server/game/Maps/MapMgr.cpp:385`
(`OnMapsJoined`), `:478` (`IsMapFrozen`); `Unit.cpp:987,8148,8435`, `SpellAuraEffects.cpp:6657`, `Spell.cpp:3775,4091`
(`Animus::Hooks::*`); `src/server/game/OutdoorPvP/OutdoorPvPMgr.cpp:49` reads `AnimusForge.Enable`.

## Runtime/ vs training

`src/server/game/Animus/Runtime/` holds the part of the sim a trained model needs to play: the movement controller,
the camera and mental map, the bot factory, the blocks, the character building and action catalog, the layout and the
stage table. Everything outside it (`AnimusForge.*`, `ForgeConfig.*`, `ForgeCommands.cpp`, Bridge, Learner, Gpu,
Console, Env, the encounters, `StageScenario`, rewards, `CurriculumTuning`) is training. The rule: **`Runtime/` never
includes a training header.** Includes are bare basenames, resolved by the core's directory-collecting CMake, so the
directory split costs nothing at build time; `apps/forge/tools/runtime_graph_check.py` resolves them the same way and
exits 1 listing every `Runtime/` file that includes a file outside it (run it before committing a change under
`Animus/`). Sub-paths below `Animus/` are kept (`Animus/Movement/Client.h` is `Animus/Runtime/Movement/Client.h`).
Counts at the split: 114 files / 25,666 lines under `Runtime/` (113 moved, plus `ActionTuning.h`), 106 files /
36,773 lines of training.

Three headers carry the seams: `Layout/ActionTuning.h` (`ActionTuning`, `OptionTuning`; `CurriculumTuning` aliases
them), `Stages/StageDefinition.h` (`ROLE_TANK/HEALER/DAMAGE`, `DrilledRole`; `RolesDraw` re-exports them) and
`Character/GearBuilder.h` (`WarmGearCaches`; `WarmCaches.h` includes it).

## Tick order (what runs when)

One world tick (`World::Update`, `src/server/game/World/World.cpp`):

1. `sAnimusForge->OnWorldPrologue(diff)` (World.cpp:1233) on the world thread: advances the group's episode clocks,
   decides whether this tick ends a decision, opens it (`EnvPool::BeginDecision`).
2. `sMapMgr->Update(diff)`: each map's task runs `OnMapPrologue` (apply the last decision's actions, sub-tick),
   `Map::Update`, `OnMapEpilogue` (reward and observe the map's envs; a map-thread reset of ended episodes may be scheduled
   as extra work) on a map-updater thread (`MapUpdater.cpp:219-222`). After the join `OnMapsJoined` (only under
   `ObserveAfterJoin`).
3. `ProcessCliCommands()` (World.cpp:1258): console and SOAP commands run here, on the world thread.
4. `sAnimusForge->OnUpdate(diff)` (World.cpp:1261): requests are applied, the decision is closed (`FinishCollect`) and
   exchanged with the learner (STEP out, ACT in), or random actions are chosen.

## Threading and ownership

| Object | Threads that touch it | Synchronisation |
|---|---|---|
| `Forge` fields (state, plan, counters, `_pool`, `_scenario`) | world thread only | none needed; commands run on the world thread (`ProcessCliCommands`) |
| `_decisionTick`, `_applyTick`, `_tickDiff`, `_turn`, `_halfBatch` | written by world thread in `OnWorldPrologue`; read by map tasks | plain fields; the scheduler's release on publishing a task and acquire on claiming it, the join in `MapUpdater::wait()` orders the next write (comment `A/AnimusForge.h:427-437`) |
| `_heldObserve` | map threads push (`OnMapEpilogue`), world thread swaps (`OnMapsJoined`) | `_heldLock` (`A/AnimusForge.cpp:340`, `:349`) |
| `EnvPool` SoA buffers (`Obs`, `Rewards`, `Done`, ...) | each map thread writes the rows of the envs on its map during its task; the world thread reads/writes between updates | disjoint rows per map; the join orders them |
| `EnvPool::_agents/_allies/_envByInstance/_mapEnvs/_envMapKey` | read by every map thread (hooks, `ApplyActionsForMap`); written by the world thread only, through `ResetDefer` when a reset ran on a map thread | no lock: "changed only while no map updates" (`A/Env/EnvPool.h:336-351`) |
| `EnvPool::_envCollect[e]`, `_observed[e]`, `_finishedOnMap[e]` | one map thread per env | per-env slot |
| `EnvPool::_applyNs` | all map threads | `std::atomic` |
| `EnvPool::_seedLock`, `_reportLock` | map threads and world thread (resets and episode reports) | mutexes |
| `ResetDefer` queue | map threads push; world thread flushes | `thread_local` flag + global mutex (`A/Env/ResetDefer.cpp:25-27`); flushed in `FinishCollect`, `ResetAll`, `Teardown` |
| `CurrentReset` | thread that resets | `thread_local` (`A/Env/ResetTiming.h:52`) |
| `RecentResets` | world and map threads | mutex ring (`ResetTiming.h:154`) |
| `Hooks::ActivePoolPtr` | map threads read, world thread sets | `std::atomic` acquire/release (`A/Env/AnimusHooks.cpp:27`) |
| `LockstepServer` | world thread | blocking; waits poll 200 ms and call `onIdle` (`A/Bridge/LockstepServer.cpp:38`) |
| `ClusterLink` | world thread | non-blocking sockets, polled; but `ConnectToHost` does a blocking `connect` (see Observed issues) |
| `LearnerProcess`/`ChildProcess` | world thread; `Stop` blocks it up to 3 x grace | polling `waitpid` |
| CLI thread, SOAP thread | only enqueue `CliCommandHolder`s (`CliRunnable.cpp:231`, `ACSoap.cpp:121`) | the world thread runs them; `Pump()` re-enters `ProcessCliCommands` while a decision waits for the learner, guarded by `_pumping` (`A/AnimusForge.cpp:1045-1056`) |
| `Gpu::Api()` | world thread | `Gpu::Load` once (`std::call_once`) |
| thread-local RNG | each thread | `Random.cpp:25` is `thread_local`; `rand_seed` reseeds the calling thread only |
| `ForgeCore::TickMs` | world thread writes, `ForgeMain` loop reads | `std::atomic` relaxed |
| `Map::DetailedObjectTiming` | bench sets, map threads read | atomic |

A map-thread reset (`AnimusForge.ResetOnMapThreads`, default on, `ForgeConfig.h:97`) is the only place map threads
create characters; all shared-manager work goes through `ResetDefer` (see [cpp-runtime-env.md](cpp-runtime-env.md)).

## The `Forge` class (`A/AnimusForge.h`)

`Forge::Instance()` is a function-local static (`AnimusForge.cpp:148`); `sAnimusForge` is the macro (`AnimusForge.h:582`).
Public API: `OnStartup`, `OnUpdate`, `OnShutdown`, `OnWorldPrologue`, `OnMapPrologue`, `OnMapEpilogue`, `OnMapsJoined`,
`IsMapFrozen`, `EnvsOnMap`, `IsIdle`, and the `Command*` functions listed in
[cpp-runtime-console.md](cpp-runtime-console.md). The module is always compiled in; `AnimusForge.Enable = 0` makes every
hook return at the top (`AnimusForge.cpp:256`, `:384`).

### State machine

`State` (`AnimusForge.h:69`): `Idle`, `Training` (remote policy, lock-step with the learner), `Running` (local policy,
the `random` policy only), `Paused`. `Request` (`:77`): `None`, `Start`, `Cancel`, `Skip`. Commands only record a request
(plus `_pauseRequested`, `_resumeRequested`); `OnUpdate` applies them at the start of a tick through `ApplyRequest`
(`AnimusForge.cpp:412`, body `:561`). The exception is a decision in progress: `RemoteDecision` passes an `onIdle`
callback that runs `Pump()` and returns `_request == Request::None`, so a pending request ends the wait for the learner
(`:2442-2447`).

Transitions:

| From | Event | To | Where |
|---|---|---|---|
| Idle | `Request::Start` | Training / Running | `ApplyRequest` -> `StartCurrent` sets `_state` (`AnimusForge.cpp:841`); a failed start -> `EndPlan` -> Idle |
| Training/Running | `_pauseRequested` | Paused (remembers `_pausedFrom`) | `OnUpdate` `:422-429` |
| Paused | `_resumeRequested` | `_pausedFrom` | `HoldWhilePaused` `:609-620` |
| any non-Idle | `Request::Cancel` | Idle | marks entry `Cancelled`, `TeardownScenario(true)`, `EndPlan` (`:583-594`) |
| any non-Idle | `Request::Skip` | next entry or Idle | `FinishCurrent(Skipped)` (`:596-598`) |
| Training | learner finished by itself (exit 0) | next entry | `RemoteDecision` -> `FinishCurrent(Done)` (`:2464-2468`) |
| Running | `LocalEpisodes` reached | next entry | `LocalDecision` (`:2414-2418`) |
| Training/Running | cluster STOP from host (worker) | Idle | `PollCluster` sets `Request::Cancel` (`:1220-1226`) |

While Paused the world thread loops inside `HoldWhilePaused`, sleeping `IDLE_SLEEP` = 50 ms and calling
`_learner.Poll()` and `Pump()`, so console commands still run; an Idle sim sleeps 50 ms per tick
(`AnimusForge.cpp:104`, `:450`). A pause does not reach cluster workers: `CommandPause` (`ForgeCommands.cpp:695`) sends
nothing to `_cluster`; the workers' sims simply block waiting for the host's learner.

### Plan, entries and outcomes

`Plan` (`AnimusForge.h:147`): `Entries` (`PlanEntry`: scenario name, `Resume`, `Result` `Outcome`, optional `Config`
override and `MapThreads` for a bench trial), `Index`, `Policy` ("remote" trains; anything else is a local policy and
only `random` exists, `KnowsPolicy` `AnimusForge.cpp:2741`), `LocalEpisodes`, `Fast`, `Budget`. `_requested` is the plan a
command queued, `_plan` the running one, `_lastPlan` the last that ended (for `forge resume` with no names).
`Outcome`: `None`, `Done`, `Skipped`, `Failed`, `Cancelled`.

`StartCurrent` (`AnimusForge.cpp:663-843`) for the current entry:

1. builds the scenario with `Animus::CreateScenario(name, config.Stage(name))`; an unknown name fails the plan; a scenario
   whose `Playable()` is false (no class of the run can play it) is marked `Skipped` and the loop moves on, ending the
   plan if it was the last (`:669-694`);
2. refuses a vision stage under `ObserveAfterJoin` (`:716-724`);
3. applies a bench trial's map-thread count (`:727`);
4. creates `EnvPool(scenario, config.Stage(name))`, `Setup()` (every env `Scenario::Setup`) (`:730-733`);
5. remote policy: `LockstepServer::Listen(SocketPath)`; on a cluster host takes the registered workers and
   `DealClusterLearners` (`:748-754`); starts the learner process unless `Learner.AutoStart = 0`, in which case it logs
   the manual command (`:756-767`);
6. `_pool->ResetAll()`; sets the stage's split `_runTicks = config.TicksFor(name)`, `_halfBatch`, `_runWorldTickMs =
   max(1, DecisionMs / _runTicks / (halfBatch ? 2 : 1))` and `ForgeCore::SetTickMs(_runWorldTickMs)` (`:777-786`); falls
   back to one group when the envs do not split on map boundaries (`:787-793`);
7. zeroes every counter, `_monitor.Begin(name)`, `Hooks::SetActivePool(pool)`, sets the state (`:798-842`).

`TeardownScenario(stopLearner)` (`:845`): detaches the pool from the hooks, `ExpectExit` + `DropClient` + `Stop(15 s grace)`
on the learner when asked, ends evaluation videos, `Teardown` + reset of pool and scenario. `FinishCurrent(outcome)` (`:886`):
records the outcome, prints the stage-end report, tears down (the learner is stopped unless the outcome is `Done`, or a bench
trial), advances `_plan.Index`, starts the next entry or `EndPlan`. `EndPlan` (`:916`): Idle, `SetTickMs(0)`, copies
`_plan` to `_lastPlan`, broadcasts `STOP` if host, ends a bench phase or prints the outcome table.

Start does not resume: `forge start` queues entries with `Resume = false`; the learner archives the previous run
(`ForgeCommands.cpp:490`). `forge resume` with names resumes only the first named entry, the rest start fresh
(`:666`); with none it takes the first entry of `_lastPlan` that is not `Done`/`Skipped` and resumes only that one
(`:631-650`). `forge resume` while Training with a dead learner restarts only the learner (`:578-607`).

### Per-tick bookkeeping

`OnWorldPrologue` (`:251`) computes `_turn` (half-batch alternates groups), advances clocks with `AdvanceClock(_turn,
diff or 2*diff)`, and sets `_decisionTick = ++_ticksSinceDecision >= _runTicks`. `_applyTick` is set from `_actionsPending[_turn]`
so the maps of exactly one tick apply a decision's actions (`:306`). `OnUpdate` (`:382`) accounts wall time into
`_worldNs` (between the end of the previous update and the start of this), `_simNs` (the module's own time minus waiting
on the learner) and `_learnerNs`; polls export and cluster; auto-tunes if configured; applies requests; closes the
decision (`_ticks` counts decisions, not world updates: only after the last group in half-batch, `counted`), calls
`MaybeReport`, `WatchResets`, `MaybeAuditCamera`, then `RemoteDecision(_turn)` or `LocalDecision(_turn)`, and sums the pool's
per-decision timings into `_collect` (`:471-531`).

It logs once an error if the tick it was handed differs from `_runWorldTickMs` (a stale worldserver) (`:459-467`).
`ForgeMain` computes the same tick from the same keys independently (see [cpp-runtime-process-gpu.md](cpp-runtime-process-gpu.md)).

### Evaluation, replay, stand-in

All driven by the learner's messages inside `RemoteDecision` (see the bridge document): `MODE` (training/evaluation, seed
base, first seed, episodes, held-out arena, optional baseline name, `MODE_FLAG_STAND_IN`), `WEIGHTS`, `PROGRESS`,
`REPLAY`. `ApplyMode` (`:2746`) rejects `Mode > 1`, an unknown baseline, and applies
`PinEvaluationArena`, `SetStandIn`, `SetEvaluation`. A baseline can only be `random` (`KnowsPolicy`); with one set the
learner's ACT is overwritten by `ChooseLocalActions` (`:2726-2734`). Evaluation videos: `BeginEvalVideos` (`:1533`) picks
seeds, `CaptureEvalVideos` (`:1590`) feeds frames before each STEP leaves; both are skipped for a baseline run. The camera
audit (`MaybeAuditCamera`, `:1398`) writes PNGs and `audit.csv` under `runs/<scenario>/camera/` every
`Vision.AuditInterval` real seconds. `WatchResets` (`:1661`) logs a "Reset stall" warning every 30 s check (re-logged
at most every 300 s) using `ResetStallText`.

### Benchmark (`forge bench`)

Phase 1 runs one plan entry per (map threads x envs) pair with the `random` policy (`BenchPlan`, `:1761`), each entry
carrying `ForgeConfig::BenchProfile(...)` (own output dir `<OutputDir>/bench`, a learner that only trains). `BenchTick`
(`:1782`) is called when a decision is counted: at `_ticks == warmupTicks` it stamps counters; at `warmup + measure` it
fills the `BenchTrial`, may drop bigger trials when memory use exceeds `Bench.MaxMemoryPercent`, and `FinishCurrent(Done)`.
`BenchPlanEnded` (`:1910`) starts phase 2 (the best `Bench.LearnerTop` settings x `Bench.LearnerTorchThreads` with the
learner) unless the sim is local-only, then `BenchReport`, `BenchSave` (`bench.json`: trials plus "best", `cpu` signature
`CpuSignature()` = FNV-1a of the first CPU's vendor/family/model/flags, `:1705`), optionally `ApplyBenchResult`, `BenchEnd`
(restores `MapUpdate.Threads`). The winner prefers learner trials over sim-only ones (`:2016`, `:2123`).
`ApplyBenchResult` (`ForgeCommands.cpp:989`) writes `MapUpdate.Threads` into `worldserver.conf` and `AnimusForge.Envs` /
`AnimusForge.Learner.TorchThreads` into the legacy `modules/mod_animus_forge.conf` if it exists, else `worldserver.conf`
(`ModuleConfigFile`, `:121`), keeping a `.before-bench` copy. `AnimusForge.Bench.AutoTune` runs `forge bench auto` once per
start when `bench.json` has no entry for this CPU (`OnUpdate` `:398-411`; `HasBenchForThisCpu` `:1730`); a worker that tunes
itself joins its host only afterwards (`_joinAfterBench`).

No tests (removed 2026-10-07); see [tests.md](tests.md).

## Observed issues

- `A/AnimusForge.h:395`: the comment block about half-batch `_turn/_nextTurn` sits above `ClusterLink _cluster`, not above
  the fields it describes (`:421-423`). Same for `:257-258` (a `WorkerPlan` comment before `ClusterRank`).
- `A/AnimusForge.cpp:997-1001`: the doc comment mentions `stage19_duo_led` and `stage15_arena`, deleted stages (also
  `A/Env/EnvPool.cpp:889`: `stage15_arena`, `stage17_flag`).
- `A/ForgeCommands.cpp:20`: header comment points to `Hooks/ForgeCommandScript.cpp`; the table is
  `src/server/scripts/Commands/cs_forge.cpp`.
- `A/Env/EnvPool.h:48`: "fed by the library's scripts while the pool is registered (PoolRegistry)": the mechanism is
  `Hooks::SetActivePool`.
- `A/Scenario/Scenario.h:80`: "All calls happen on the world thread, outside MapMgr::Update" is false for `ApplyActions`,
  `ApplyGoals`, `ApplyLook`, `SubTick`, `Reward`, `IsTerminal`, `Observe` and map-thread `Reset` (called from
  `EnvPool::ApplyActionsForMap`/`ObserveMap`/`ResetMapEnvs`).
- `A/Env/EnvPool.cpp:677-679`: "the world thread's random numbers" is stale: under `ResetOnMapThreads` the thread-local
  RNG of the map thread that resets is reseeded (`Random.cpp:25`).
- `A/ForgeConfig.h:228` and `A/ForgeCommands.cpp:1284`: `ForgeConfig::Level` is never set to a non-zero value
  (`FastProfile` deliberately does not, `ForgeConfig.cpp:719-721`); the "level N" branch of `FastSummary` is dead.
- `A/ForgeConfig.h:252` `Bench.MaxEnvs = 256` vs `ForgeConfig.cpp:449` default 512 (and conf.dist 512).
- `A/AnimusForge.cpp:1098`: auto-restart of failed learners requires `_clusterLearners != 0`; a host whose workers run only
  sims gets no auto-restart (`_autoResumes` path unreachable). Reviewer question: intended?
- `A/AnimusForge.cpp:1354`: `static std::atomic<bool> warned` in `WorkerPlan` never resets, so a tick-mismatch warning is
  given once per process.
- `A/AnimusForge.cpp:2826-2836,2879-2905`: a rank's device buffers are freed only on the next `OfferDevice` or a declined
  answer, never in `TeardownScenario`/`OnShutdown`.
- `A/Env/EnvPool.h:346`: `EnvPool::_envByInstance` (`EnvPool.cpp:770`) is added to at every `IndexEnv` and cleared only in
  `Teardown`; whether instance ids recycle so that it stays bounded is UNVERIFIED.
- (fixed 2026-10-08) `A/Json/BoostJson.cpp:27`: the `#error` text named "mod-animus-lib".
- Retained surface after principle 14 (no baselines): `MODE.Baseline`, `EvalBaseline()`, `ChooseLocalActions`, the
  `baseline_score` rows in `Progress.cpp:576-585` and `Bench.Policy` exist for the `random` policy only.
- (fixed 2026-10-08) `RemoteAccess/` was deleted, with the `Ra.*` keys of `worldserver.conf.dist`.
- Memory notes say the SOAP listener never starts; the code starts it when `SOAP.Enabled` is set (`ForgeMain.cpp:475`).
