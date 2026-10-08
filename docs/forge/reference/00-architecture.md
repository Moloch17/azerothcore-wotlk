# Architecture of the forge

Purpose and scope. This is the whole system on a few pages, then in depth: processes, the tick and the decision loop,
the observation and action path from the sim to the wire to the network and back, the episode and stage lifecycle,
the cluster, threading, where state lives, and an ASCII data-flow diagram. It describes what the code does as of
`forge` at `bd32b9dc8`. The deep units (blocks, rewards, encounters, movement, vision, the learner's algorithm, the
wire formats) are documented in sibling files and only summarised here:
[01-forge-core-delta.md](01-forge-core-delta.md) (what differs from upstream), [cpp-runtime.md](cpp-runtime.md),
[cpp-movement.md](cpp-movement.md), [cpp-vision.md](cpp-vision.md), [cpp-blocks.md](cpp-blocks.md),
[cpp-layout-character.md](cpp-layout-character.md), [cpp-rewards-routing.md](cpp-rewards-routing.md),
[cpp-encounters.md](cpp-encounters.md), [cpp-stagescenario.md](cpp-stagescenario.md),
[cpp-tuning-keys.md](cpp-tuning-keys.md), [py-learner.md](py-learner.md), [py-mappo.md](py-mappo.md),
[py-human-and-misc.md](py-human-and-misc.md), [protocol.md](protocol.md), [file-formats.md](file-formats.md),
[metrics.md](metrics.md), [config-keys.md](config-keys.md), [config-yaml.md](config-yaml.md), [tests.md](tests.md),
[tools-and-ops.md](tools-and-ops.md), [stages.md](stages.md), [known-issues.md](known-issues.md),
[glossary.md](glossary.md).

## The area this file maps

The orchestration layer: everything that starts the process, ticks the world, trades STEP and ACT with the learner,
runs plans, and talks to other machines. The map table lists it; every file of these directories is in it
(checked by script, see the end of the document):

- C++: `ForgeMain.cpp`, `Forge/*`, `Animus/AnimusForge.*`, `ForgeConfig.*`, `ForgeCommands.cpp`, `Bridge/*`,
  `Learner/*`, `Env/*`, `Console/*`, `Json/*`, `cs_forge.cpp`, and the tick parts of `World.cpp`, `MapMgr.cpp`,
  `MapUpdater.*`.
- Python: `train.py`, `env.py`, `protocol.py`, `parallel.py`, `async_sync.py`, `stage.py`, `stages.py`, `runs.py`,
  `progress.py` under `apps/forge/python/animus/`.
- Operations: `forge.sh`, `forgectl` (shim and package), `apps/docker/forge-worldserver.sh`, `animus-venv.sh`,
  `docker-compose*.yml`, `apps/forge/cluster.toml`.

### Map table

| Path | Lines | Role |
|---|---|---|
| `src/server/apps/worldserver/ForgeMain.cpp` | 520 | `main()`: startup order, the two world loops, the database seal, shutdown |
| `src/server/game/Forge/Forge.h` | 51 | `ForgeCore` namespace: HasClients, per-stage tick override |
| `src/server/game/Forge/Forge.cpp` | 60 | `ForgeCore` definitions (a bool and an atomic tick) |
| `src/server/game/World/World.cpp` | 1804 | `World::Update`, the fixed-tick world step that calls the module (only its tick part is in scope) |
| `src/server/game/Maps/MapMgr.cpp` | 668 | `MapMgr::Update`: schedules one task per map, join, timing, replicas |
| `src/server/game/Maps/MapUpdater.h` | 128 | map task pool interface |
| `src/server/game/Maps/MapUpdater.cpp` | 349 | pinned lock-free task pool; `RunMapTick` calls the module's per-map halves |
| `src/server/scripts/Commands/cs_forge.cpp` | 1256 | the `forge` console command table and its help |
| `src/server/game/Animus/AnimusForge.h` | 584 | `AnimusForge::Forge` singleton: plan/state machine, decision loop, bench, cluster glue (class and members) |
| `src/server/game/Animus/AnimusForge.cpp` | 3087 | implementation of the above: startup, prologue/epilogue, plans, RemoteDecision, SendStep, cluster, bench, snapshot |
| `src/server/game/Animus/ForgeConfig.h` | 318 | `ForgeConfig`: every `AnimusForge.*` setting as a struct, profiles for bench and fast |
| `src/server/game/Animus/ForgeConfig.cpp` | 763 | reads and validates the config; GPU mode, bench grids, `Stage()` settings |
| `src/server/game/Animus/ForgeCommands.cpp` | 1304 | the `Forge::Command*` members behind each console command |
| `src/server/game/Animus/Bridge/Protocol.h` | 401 | wire protocol version 25: message types and structs, ACT cutting |
| `src/server/game/Animus/Bridge/LockstepServer.h` | 95 | blocking Unix/TCP socket server for learner ranks |
| `src/server/game/Animus/Bridge/LockstepServer.cpp` | 396 | listener, accept with HELLO, send/receive with polling for world stop |
| `src/server/game/Animus/Bridge/ClusterLink.h` | 158 | cluster control channel (host and worker lines, fingerprint) |
| `src/server/game/Animus/Bridge/ClusterLink.cpp` | 463 | non-blocking control sockets, registration, reconnect |
| `src/server/game/Animus/Learner/ChildProcess.h` | 83 | a child process with a log file (learner, export) |
| `src/server/game/Animus/Learner/ChildProcess.cpp` | 171 | fork/exec, reaping, SIGINT/SIGKILL escalation |
| `src/server/game/Animus/Learner/LearnerProcess.h` | 82 | the Python learner as one or more rank processes |
| `src/server/game/Animus/Learner/LearnerProcess.cpp` | 328 | builds the learner command line, CPU pinning, rank devices |
| `src/server/game/Animus/Env/Env.h` | 170 | `Env` (one environment) and `AgentStats` |
| `src/server/game/Animus/Env/Env.cpp` | 64 | `Env::FindMap/FindBot/FindTarget` |
| `src/server/game/Animus/Env/EnvPool.h` | 394 | `EnvPool`: all envs of a scenario and the flat arrays of a STEP |
| `src/server/game/Animus/Env/EnvPool.cpp` | 942 | decision phases, resets, evaluation seeding, hooks that record combat |
| `src/server/game/Animus/Env/AnimusHooks.h` | 53 | `Animus::Hooks`: damage, heal, cast events from the core |
| `src/server/game/Animus/Env/AnimusHooks.cpp` | 71 | forwards to the active pool |
| `src/server/game/Animus/Env/Kinematics.h` | 102 | the per-agent kinematic sample carried in a STEP (10 floats) |
| `src/server/game/Animus/Env/ResetDefer.h` | 58 | queue for world-thread work done during a map-thread reset |
| `src/server/game/Animus/Env/ResetDefer.cpp` | 66 | its implementation |
| `src/server/game/Animus/Env/ResetTiming.h` | 219 | timing of episode resets and the stall summary |
| `src/server/game/Animus/Console/Progress.h` | 290 | `ProgressFile`, `SimSnapshot`, the report types |
| `src/server/game/Animus/Console/Progress.cpp` | 835 | `forge status` and the periodic report; reads `progress.json` |
| `src/server/game/Animus/Console/TextTable.h` | 100 | console text table and `LineSink` |
| `src/server/game/Animus/Console/TextTable.cpp` | 188 | its implementation |
| `src/server/game/Animus/Json/BoostJson.cpp` | 30 | single translation unit that compiles header-only Boost.JSON |
| `apps/forge/python/animus/train.py` | 2309 | the learner: `TrainingRun`, rollout loop, evaluation, finish |
| `apps/forge/python/animus/env.py` | 515 | `ForgeEnv` and `ClusterEnv`: the learner's end of the socket |
| `apps/forge/python/animus/protocol.py` | 558 | wire structs mirrored from `Protocol.h` |
| `apps/forge/python/animus/parallel.py` | 204 | data-parallel ranks over torch.distributed |
| `apps/forge/python/animus/async_sync.py` | 450 | asynchronous cross-machine weight exchange (Hub/Link) |
| `apps/forge/python/animus/stage.py` | 829 | `ConvergenceController`, shaping fade and cost ladder |
| `apps/forge/python/animus/stages.py` | 115 | reads the sim's `stage.json` |
| `apps/forge/python/animus/runs.py` | 100 | run directories: archive, prune, resume check |
| `apps/forge/python/animus/progress.py` | 165 | writes `progress.json` |
| `forge.sh` | 109 | start/attach/stop the worldserver container |
| `forgectl` | 8 | shim that runs the forgectl package |
| `apps/docker/forge-worldserver.sh` | 95 | container command of `ac-worldserver` |
| `apps/docker/animus-venv.sh` | 42 | creates the learner's Python venv |
| `docker-compose.yml` | 284 | the compose project |
| `docker-compose.cluster.yml` | 25 | host networking overlay for cluster machines |
| `apps/forge/cluster.toml` | 75 | machines, ports, containers read by forgectl |
| `apps/forge/forgectl/__main__.py` | 185 | forgectl: command line entry |
| `apps/forge/forgectl/audit.py` | 142 | forgectl: audit log |
| `apps/forge/forgectl/cluster.py` | 153 | forgectl: `forgectl cluster` |
| `apps/forge/forgectl/config.py` | 135 | forgectl: reads cluster.toml |
| `apps/forge/forgectl/confsync.py` | 249 | forgectl: `conf-sync` |
| `apps/forge/forgectl/console.py` | 275 | forgectl: types lines into a worldserver console |
| `apps/forge/forgectl/deploy.py` | 348 | forgectl: `build`, `move-host` |
| `apps/forge/forgectl/home.py` | 10 | forgectl: `~/.forgectl` |
| `apps/forge/forgectl/logs.py` | 96 | forgectl: `forgectl logs` |
| `apps/forge/forgectl/remote.py` | 84 | forgectl: ssh helper |
| `apps/forge/forgectl/stage.py` | 233 | forgectl: `forgectl stage` |
| `apps/forge/forgectl/testcmd.py` | 165 | forgectl: `forgectl test` |
| `apps/forge/forgectl/ui.py` | 58 | forgectl: prompts and tables |
| `apps/forge/forgectl/videos.py` | 48 | forgectl: `forgectl videos` |
| `apps/forge/forgectl/__init__.py` | 1 | forgectl: package marker |

Everything else under `src/server/game/Animus` (Bot, Gpu, Movement, Scenario, Vision) and the rest of
`apps/forge/python/animus` belongs to the sibling documents.

## 1. The system on one page

The forge is a modified AzerothCore worldserver that never talks to a game client. Its job is to run many small
copies of the game (envs), each holding one to forty in-process "bots" (seats), at a fixed game-time step, and to
exchange observations and actions with a Python MAPPO learner once per decision. The learner trains one policy per
class and decides when each stage is done.

Processes:

| Process | What | Started by | Talks to |
|---|---|---|---|
| worldserver (the forge) | C++; world thread plus map-update worker threads; owns the sim, the plans and the console | the container command (`forge-worldserver.sh`, `exec ./worldserver`) | learner (socket), cluster peers (TCP), console (stdin), optionally SOAP |
| learner | Python `python -m animus.train`, one process per rank | the worldserver (`LearnerProcess::Start`) | its sim (Unix socket or TCP), other ranks (torch.distributed or the async hub) |
| export | Python `animus.export` | `forge export` (`ForgeCommands.cpp:1059`) | reads a checkpoint, writes `.amdl` + manifest |
| cluster workers | the same worldserver with `AnimusForge.Cluster.Role = "worker"` | the container on another machine | the host's control port and learner |
| host | the worldserver with `Role = "host"` | same | its workers |
| dev tools | `forgectl`, `forge.sh`, `apps/forge/tools/*`, TensorBoard | a person | ssh, `docker`, console |

One decision is `AnimusForge.DecisionMs` of game time (default 250). The world ticks in `DecisionMs /
TicksPerDecision` steps (defaults 250 and 1). Per decision, for every env: its seats' actions are applied, the world
ticks, the transition is scored (reward, done), the observation is built, an ended episode is rebuilt, and the
whole pool is sent to the learner, which answers with the next actions. The world thread blocks while it waits
(lock-step), but keeps answering the console.

State lives in memory. After startup the three MySQL pools are sealed (writes are dropped, reads return nothing);
what persists is files the learner and the sim write under `AnimusForge.OutputDir`.

## 2. Startup (trace from `ForgeMain.cpp`)

1. `main` (`ForgeMain.cpp:337`): fatal-signal backtrace handler, `ConfigMgr::Configure`, `LoadAppConfigs`,
   the legacy `modules/mod_animus_forge.conf` is merged if it exists (`:361`). `sLog` is synchronous and has no database appender.
2. One IoContext thread for SIGINT/SIGTERM; process priority; module configs; scripts (`AddScripts`,
   `AddModulesScripts`).
3. `ForgeStartDB` (`:166`): `DatabaseLoader` over the three databases (DB updates run here), `realm.Id.Realm` from
   `RealmID`.
4. `sWorld->SetInitialWorldSettings()` (`:417`): loads every DBC and world table. The forge's `ForgeConfig` is not
   read yet.
5. (No world listener is started.)
6. `sScriptMgr->OnStartup()`, then **`sAnimusForge->OnStartup()`** (`AnimusForge.cpp:154`): `ForgeConfig::Load`;
   configures the camera, mental map, entity memory (`Animus::Vision::Configure*`); configures the layered-field store
   (`LayeredField::Store::Configure(ProbeDir, ProbeCacheGrids)`); `Gpu::PrepareEnvironment`; builds the fast profile;
   loads the device library if `Gpu.Observe` and the policy is remote; for a cluster machine computes the fingerprint
   (`ClusterFingerprint`, `AnimusForge.cpp:70`) and starts `Listen` (host) or `Join` (worker, unless it first runs an
   automatic benchmark); **`Curriculum::WarmCaches()`** (reads every world table the curriculum will need); opens the
   learner socket (`_server.Listen`) for a non-worker with a remote policy. It ends idle.
7. `ForgeSealDatabases()` (`:233`): `AccountMgr::LoadSnapshot()`, then `Seal(strict)` on login,
   character and world pools. After this no SQL runs (see [01-forge-core-delta.md](01-forge-core-delta.md) C).
8. SOAP thread if `SOAP.Enabled`; CLI thread if stdin is a tty.
9. `ForgeUpdateLoop` (`:266`) until `World::IsStopped()`.

`AnimusForge.Enable = 0` makes `OnStartup` stop after the camera logging (`:182-186`); `OnUpdate` and `OnWorldPrologue`
return at once. (`OutdoorPvPMgr` also keys on this setting.)

## 3. The tick and the decision loop

The world thread runs this, every tick (fixed diff `D = ForgeCore::TickMs()` or the configured tick):

```
ForgeUpdateLoop                                     ForgeMain.cpp:266
 sWorld->Update(D)                                   World.cpp:1150
  GameTime::AdvanceGameTimers(D)                     clock += D
  [timed resets, auctions, LFG 0]
  sAnimusForge->OnWorldPrologue(D)                   AnimusForge.cpp:251
      _turn, _pool->AdvanceClock(group, D)           episode clocks move every tick
      _decisionTick = ++_ticksSinceDecision >= _runTicks
      if decision tick: _pool->BeginDecision(group)
      _applyTick = _actionsPending[group]            the last decision's actions land on THIS tick
  sMapMgr->Update(D)                                 MapMgr.cpp:335
      for each map: ForgeTickDiff -> MapUpdater task        (frozen half-batch maps get 0)
      MapUpdater::RunMapTick(map)                    one task per map, any worker thread
          OnMapPrologue(map)                         ApplyActionsForMap if _applyTick; SubTickMap if tickDiff
          Map::Update, Map::DelayedUpdate            the game itself
          OnMapEpilogue(map)                         if _decisionTick: ObserveMap (reward, done, observe,
                                                     reset-on-map-thread)
      wait() (world thread works too)
      OnMapsJoined                                    only with ObserveAfterJoin
      LoadDeferredTiles, ForgeTrimHeap, timing roll-up
  [battlegrounds, outdoor PvP, world state, LFG 2, query callbacks, instance saves]
  ProcessCliCommands
  sAnimusForge->OnUpdate(D)                          AnimusForge.cpp:382
      PollExport, PollCluster, auto-tune, ApplyRequest (start/cancel/skip), pause
      if decision tick closed this update:
          MaybeReport, WatchResets, MaybeAuditCamera
          RemoteDecision(group)  |  LocalDecision(group)
  sScriptMgr->OnWorldUpdate; MySQL ping
```

Key points a reviewer must hold on to:

- **Actions arrive one tick late by construction.** `RemoteDecision` stores the ACT into `_pool->Actions` and sets
  `_actionsPending[group]` (`AnimusForge.cpp:2738`); the next `OnWorldPrologue` turns that into `_applyTick`
  (`:306`); the map tasks of that tick apply them (`EnvPool::ApplyActionsForMap`) before the tick runs. So the
  world advances while the learner thinks about nothing: the world thread is blocked in `RemoteDecision` and no map
  runs. The "lock-step" is strict; there is no overlap except half-batch (below).
- **Scoring and observation run on the map threads**, in `OnMapEpilogue` -> `EnvPool::ObserveMap` -> `ObserveEnv`
  (`EnvPool.cpp:241`): `Scenario::Reward`, step-stat roll-up, the `done` test (`IsTerminal` or `EpisodeElapsedMs >=
  EpisodeLengthMs`), then `Observe` (or the final observation of an ended episode). An ended episode is rebuilt on the
  map thread when `ResetOnMapThreads` and the scenario's `ResetsStayOnMap()` both allow it (`EnvPool.cpp:48,274`);
  otherwise `FinishCollect` rebuilds it serially on the world thread (`:406`).
- **`FinishCollect` closes the decision** on the world thread (`RemoteDecision` calls it before `SendStep`,
  `AnimusForge.cpp:2496`): it flushes `ResetDefer`, scores any env whose map did not tick (logging once as a
  scheduler bug), rebuilds the remaining ended episodes and totals the timing.
- **`TicksPerDecision` (T) above 1**: the world ticks `DecisionMs / T` per update; only every T-th tick is a decision.
  Between decisions `SubTickMap` moves the seats' controllers (`StageScenario::SubTick`, `StageScenario.cpp:2832`) and
  nothing is observed or sent. The module checks that the diff it receives equals `_runWorldTickMs` and logs an error
  once when not (`AnimusForge.cpp:464`). A stage may set its own T (`AnimusForge.Stage.<name>.TicksPerDecision`);
  `StartCurrent` publishes it with `ForgeCore::SetTickMs` (`:780`) and `EndPlan` clears it (`:919`). The first update
  of a run was sized with the old tick (`_tickJustSet`).
- **Half-batch** (`AnimusForge.HalfBatch`, only with T = 1): the pool is two groups; the world ticks at half a
  decision, each group's maps ticking every other world tick with both ticks' time (`ForgeTickDiff`), while the learner
  decides the other group. `RemoteDecision` waits only for the group whose maps tick next (`target`). Falls back to one
  group when a map holds envs of both (`AnimusForge.cpp:786-792`). A learner with a cast or partners never pipelines.
- **Idle and paused**: `OnUpdate` abandons a decision opened by the prologue (`abandonDecision`) and sleeps 50 ms per
  tick when idle; `HoldWhilePaused` loops serving the console.

### While the world thread waits for the learner

`RemoteDecision` (`AnimusForge.cpp:2438`) blocks in `LockstepServer::AcceptClients` or `ReceiveAny`; both poll with an
`onIdle` callback that polls the learner process, runs `Pump()` (`sWorld->ProcessCliCommands()`, export and cluster
polls, the periodic report) and returns false when a console command needs the plan to end (`_request != None`). So
the console stays responsive, and `forge cancel` works mid-wait. Commands only record a `Request`; `ApplyRequest`
applies it at the start of the next `OnUpdate`.

## 4. The observation and action path, end to end

Sim side (per env, per decision):

1. **Seats** are `Player` objects created without a client (`BotFactory::Create`: a socketless `WorldSession`
   flagged `SetSimSession(true)`, movement orders enabled) and placed in an instance map (`PlaceInNewInstance`) or on a
   continent replica. `BotSlot` keeps two sessions and GUIDs per seat so a new bot can be built before the old leaves.
2. **Observe**: `StageScenario::Observe` (`StageScenario.cpp:3418`) fills, per seat, an observation row (`ObsDim`
   floats, padded to the largest layout), a mask row (`NumActions` bytes), and for stages with a vision block an image
   (`ImageBytes`) and with a map block a mental-map crop (`MapBytes`); one critic state row per env (`WriteState`).
   The row is produced by the layout's blocks through `SeatEncoder` over a `SeatView`
   ([cpp-layout-character.md](cpp-layout-character.md), [cpp-blocks.md](cpp-blocks.md)).
3. **Describe**: `AgentLayouts` (which layout each seat uses), `AgentPresence` (1 present, 0 empty, 2 the stand-in),
   `AgentKinematics` (10 floats a seat) fill `Layout`, `Present`, `KinematicSamples`.
4. **Pool arrays** (`EnvPool.h`): `Obs`, `State`, `Mask`, `Rewards`, `Done`, `Terminated`, `FinalObs`, `FinalState`,
   `EpisodeInfo`, `EpisodeSeed`, `Layout`, `Present`, `KinematicSamples`, `Image`/`FinalImage`, `MapCrop`/
   `FinalMapCrop`, plus the inbound `Actions`, `Goals`, `Look`.
5. **STEP**: `Forge::SendStep` (`AnimusForge.cpp:2938`) sends, per learner rank, a `StepHeader{decision, EnvBegin,
   EnvCount}` followed by that rank's contiguous rows of every array, with the ended envs' `final_*`/`episode_info`
   gathered. If the learner accepted device buffers (`DEVICE` handshake, protocol 15), obs/state/mask (and image) are
   first copied into HIP buffers by `UploadRows` and left out of the message. The map crop always travels on the socket.
   Exact layout: [protocol.md](protocol.md).

Wire: `LockstepServer` (Unix socket path, or `tcp://host:port` for a cluster worker's sim), one blocking connection per
learner rank. Messages are `MsgHeader{type,length}` + payload; the sim sends SPEC, DEVICE, STEP; the learner HELLO,
DEVICE_ACK, ACT, MODE, WEIGHTS, PROGRESS, REPLAY, EXPLORE_STARTS, CLOSE. Version 25 (`Protocol.h`); `MODE_FLAG` bit 1
is unused.

Learner side (`train.py`): `ForgeEnv` reads SPEC and (device) DEVICE, `TrainingRun.rollout` acts group by group
(`_act_on_rows`: the actor network with the mask, goal head, look head, recurrent memory, partners and cast where
used), records the decision into the `RolloutBuffer`, sends the ACT, and the next STEP carries the transition's reward
and done. After `rollout_length` decisions it computes advantages and calls `trainer.update` (optionally overlapped on a
one-thread executor, `train.py:804`, `1768`). Details: [py-learner.md](py-learner.md), [py-mappo.md](py-mappo.md).

Back in the sim: `RemoteDecision` receives ACT (`ActHeader{EnvBegin, EnvCount}` and the cut from `CutAct`: actions,
optionally two goals a seat, optionally three look choices a seat), range-checks the look section (a bad value drops the
learner), copies into `Actions`/`Goals`/`Look`, sets `_actionsPending`. Other message types may arrive first and are
handled in the same loop without ending the wait: MODE (reset every env and answer with fresh STEPs, once every rank has
asked), WEIGHTS (`SetLayoutWeights`), PROGRESS (`SetStageProgress`, shaping scale and cost scale clamped to 0..1, NaN
is full), REPLAY, EXPLORE_STARTS; CLOSE or a protocol error drops the learner.

Applying an action: `EnvPool::ApplyActionsForMap` (map thread) -> per env `ApplyGoals`, `ApplyLook`, then
`StageScenario::ApplyActions` -> `encounter->UpdateEnemies/Update`, then `ApplySeatAction` -> `SeatEncoder::Apply`
(`StageScenario.cpp:3258`). Movement actions set held keys; casts, item use, selection, attack and object use are built
as the **client packets** (`EntityActions::CastSpell`, `UseItem`, `SetSelection`, `AttackSwing`, `GameObjectUse`,
`GossipHello`) and handed to the session's own `Handle*Opcode` (`EntityActions.cpp:43-56`). Each tick `SubTick` runs
the player controller (`Mover.Tick`), which reports to the server through `PlayerLink::Apply` ->
`ClientMovement::Apply` ([cpp-movement.md](cpp-movement.md)). The core's damage, heal and cast calls feed the pool back
through `Animus::Hooks` into `EnvPool::Record*`, which write the per-seat step stats the next `Reward` reads.

## 5. Episode and stage lifecycle

**Plan.** A console command builds a `Plan` (a list of `PlanEntry{Scenario, Resume}` and a policy name). `forge start`
without names uses `DefaultQueue()` = `AnimusForge.Queue` or every stage (all twelve), skipping
stages whose `finished.json` says advanced when `Queue.SkipFinished` (default on). `forge resume <stage>` resumes the
first entry from `latest.pt` and starts later entries from scratch. `forge resume` with no names unpauses, restarts a
dead learner, or continues `_lastPlan` from the first entry that did not end Done or Skipped.
`CommandStart/Resume` only set `_requested` and `_request = Start`.

**Start of a stage** (`StartCurrent`, `AnimusForge.cpp:663`): `CreateScenario(name, settings)` builds a
`StageScenario` from the stage's `StageDefinition` (`Stages.cpp`), which writes `<OutputDir>/layouts/<stage>/` (a layout
manifest `<model>.json` per class and `stage.json`, `StageScenario::WriteStageFiles`, `StageScenario.cpp:1240`).
A stage no run class can play is skipped (`Playable()`). Then an `EnvPool` of `Stage(stage).Envs` envs is created and
`Setup()` builds each env's first episode (`Scenario::Setup`). For a remote policy: the socket listens, a cluster host
deals the registered workers (`DealClusterLearners`), and the learner starts (`LearnerProcess::Start` with `--resume`
when asked). `ResetAll()` builds fresh observations; the stage's tick is published; `Hooks::SetActivePool`; state
becomes Training (remote) or Running (local `random`).

**The learner** (`TrainingRun.__init__`, `train.py:541`): archives any earlier run unless `--resume`
(`runs.archive_run`), connects, reads SPEC, loads `stage.json`, builds the networks, seeds from the stage's parents
by name (`bootstrap.py`, `stage.json` `seed_chain` and `merges`), restores the checkpoint when resuming, then
`run()` (`:2219`): first STEP, optional evaluation at start, `train()` (`:2187`).

**Training loop**: rollout -> update -> log -> checkpoint; every `eval.every_env_steps` an **evaluation**: the learner
sends MODE (evaluation, seed base, episodes, optional `random` baseline, stand-in flag, optional held-out arena); the
sim resets every env and plays seeded episodes (`EnvPool::SetEvaluation`: seed index `i` builds from
`rand_seed(f(base, i))` on the world thread); the learner scores them, updates `best.pt` and sets MODE back to training.
`ConvergenceController.after_eval` returns ADVANCE when every played class has converged (plateau, policy stopped
moving, entropy settled, ladder settled, at the top rung) or `at_budget` at `total_env_steps` (a ceiling).
Gate-stepped ladders re-baseline convergence at every rung (`ConvergenceController.rebaseline`, `stage.py:644`) and
write `best_rung<k>.pt` (`runs.archive_rung_best`). See [py-learner.md](py-learner.md), [stages.md](stages.md).

**End of a stage**: the learner saves `latest.pt`, writes `finished.json` (`advanced`, `reason` converged or budget,
steps), closes the socket and exits 0 (**both outcomes exit 0**). The sim sees the closed socket in `ReceiveAny`
(drops the client), then in the next `RemoteDecision` `AcceptClients` stops waiting because
`LearnerFinished()` (exit status 0 of an auto-started learner, `AnimusForge.cpp:947`), and calls
`FinishCurrent(Done)`: report, `TeardownScenario(false)`, next entry's `StartCurrent`, or `EndPlan`. Any other
ending (cancel, skip, failure) stops the learner (SIGINT after a grace of 15 s, `LEARNER_STOP_GRACE`, so it saves) with
`TeardownScenario(true)`.

**Episode**: an env's episode ends on `IsTerminal` (all seats dead, objective done, or a failed build) or on the time
limit. Terminal sets `Terminated` (no bootstrap); a limit is a truncation. The ended episode's last observation goes in
`FinalObs`; `EpisodeInfo` (per-seat totals, column names from SPEC) goes with it; the env is rebuilt
(`StageScenario::Rebuild`: draw arena, spawn, classes, levels, gear, encounters) and its first observation of the new
episode is in the same STEP's `Obs`. No stage ends at the first death: wipes are scored.

**Run directory** `<OutputDir>/runs/<stage>/` (names confirmed in `train.py`, `runs.py`, `ForgeConfig.cpp:672`):
`config.yaml`, `spec.json`, `stage.json`, `metrics.csv`, `layouts.csv`, `eval.jsonl`, `eval.csv`,
`eval_episodes.jsonl`, `eval_baseline.json`, `eval_trace.jsonl`, `eval_motion.npz`, `progress.json`, `finished.json`,
`latest.pt`, `best.pt`, `best_rung<k>.pt`, numbered `checkpoint_*.pt`, `tb/`, `events.log` (written by the sim),
`camera/`, `videos/`; archives in `<OutputDir>/archive/`. Formats: [file-formats.md](file-formats.md).

## 6. The cluster

Roles (`AnimusForge.Cluster.Role`): standalone, host, worker. The control channel (`ClusterLink`, TCP, text lines,
port 7700 by default) carries orders only; STEP/ACT go straight between a learner and a sim.

```
worker -> host   REGISTER <dataport> <addr>  FINGERPRINT <k=v ...>  CAPS learner=<0|1>  PROGRESS <k=v ...>
host   -> worker REFUSED <keys>   START <stage> <resume> <fast> [envs=] ticks=  [rank= world= dist= sync=]
                 STOP             RUNG <n>
```

- **Fingerprint** (`ClusterFingerprint`, `AnimusForge.cpp:70`): `src=` the 16-hex prefix of a SHA-256 over the SHA-256 of
  every `Animus/*.cpp`, `.h`, `.hip` file, computed by CMake at configure time (`game/CMakeLists.txt`; nothing outside
  `Animus/` and no Python is hashed), `protocol=` `PROTOCOL_VERSION`, `fields=count/bytes` of the `*.field` files in
  `ProbeDir` (a count and a byte total, **not** a content hash), `curriculum=` the FNV-1a of the serialised
  `CurriculumTuning::Load("AnimusForge.Curriculum.")` (every value in force, defaults included), `decision=`
  `DecisionMs/TicksPerDecision`. The host registers a worker only when the strings are equal and otherwise answers
  `REFUSED` with the differing keys; the worker retries a minute later.
- **START**: the host sends the stage, resume flag, fast flag, its own per-stage env cap (the worker runs at most that
  many) and its world ticks a decision (which the worker runs whatever its conf says, `WorkerPlan`). A worker builds a
  plan with a remote policy, no learner of its own (unless `Cluster.Learner`), and its sim listening on
  `tcp://0.0.0.0:<DataPort>`.
- **Learners**: the host's learner (rank 0..L-1 locally) lists the workers' sims in `cluster_sims` and its `ClusterEnv`
  (`env.py:245`) joins them into one pool, dropping and re-adding a sim that disconnects (`rejoin()` between
  rollouts). A worker that can run a learner of its own (`CAPS learner=1`) becomes another rank (`DealClusterLearners`),
  its learner started by the worker's sim with `rank`, `ranks`, `dist_address`.
- **Weight exchange** (`Cluster.Sync`, default `async`): `async` = no collective; each rank trains at its own pace and
  every `async_sync_every` updates trades its networks with the leader's `Hub` (elastic averaging weighted by
  steps, `async_sync.py`); `weights` = torch.distributed all-reduce of the networks once an update
  (`parallel.py`, gloo, or nccl when every rank has its own GPU). Both meet at `host:7702` (`ClusterDistPort`). The
  leader also serves the checkpoints a run read (`fetch_shared`) so every rank seeds identically.
- **Progress/ladder**: workers send `PROGRESS` every 5 s (rates, timing, `wing=` tally of dungeon runs). The host's
  scenario folds the tallies, decides the dungeon ladder rung, and broadcasts `RUNG` on change and every 30 s.
- **Failure**: a worker lost is dropped; back, it is ordered onto the running stage again. If a multi-machine learner
  run fails, the host restarts its learners from `latest.pt` up to three times (`PollCluster`), then waits for
  `forge resume`.
- **Pause does not cross the wire**: a worker handles only START, RUNG and STOP (`PollCluster`, `AnimusForge.cpp:1118` onward),
  so `forge pause` on the host never reaches workers; `forgectl stage pause` types the command into each console.

Operational detail: [../cluster.md](../cluster.md), [../forgectl.md](../forgectl.md), [../deploy-gate.md](../deploy-gate.md).

## 7. Threading

| Thread | Runs | Touches shared state? |
|---|---|---|
| world thread | `main` loop, `World::Update`, `OnWorldPrologue/OnUpdate`, all socket I/O with the learner, plan start/teardown, serial parts of resets, `ResetDefer::Flush`, deferred tile loads, console command execution | owns the plan state; writes `_decisionTick`, `_applyTick`, `_actionsPending` before tasks are pushed |
| map workers (N = `MapUpdate.Threads`) + the world thread inside `wait()` | `RunMapTick`: per-map prologue (apply/subtick), `Map::Update`, epilogue (reward, observe, reset-on-map) | each task touches only the envs on its map (`_mapEnvs`); the combat hooks write per-env stats of the env whose map the thread is updating; global managers go through `ResetDefer` or locks added in the core (LFG, name map) |
| CLI thread | readline, queues commands | commands run on the world thread (`ProcessCliCommands`) |
| IoContext thread | signals | none |
| SOAP thread | optional HTTP console | same queue |
| learner process(es) | rollout inference, PPO updates; an update may overlap the next rollout on a one-thread executor; async hub/link threads; torch threads pinned away from the map pool's cores (`LearnerProcess::Start`) | none shared with the sim except the socket and HIP buffers |
| GPU | optional: observation buffers written by the world thread via `libforge-gpu.so`, camera kernels; the learner's torch on the same HIP runtime | device buffers shared by IPC handle |

The plain bools `_decisionTick` and `_applyTick` are read by map tasks without atomics; the comment at
`AnimusForge.h:427-437` argues the task push/join orders them. A reviewer should re-check this reasoning on any change to
`MapUpdater`.

## 8. Where state lives

- **Memory only**: the sim (envs, characters, groups, instance state, caches), the plan, the learner's networks and
  buffers, cluster registrations. The three database pools are sealed after startup; writes are dropped and logged
  once per kind, synchronous reads return no rows (strict).
- **Files under `AnimusForge.OutputDir`** (`/azerothcore/var/animus-forge` in the container): `runs/<stage>/`,
  `archive/`, `layouts/<stage>/` (manifests, `stage.json`), `models/` (exports), `fast/`, `bench/`.
- **Per machine, untracked**: `env/dist/etc/modules/mod_animus_forge.conf` (and `worldserver.conf`), the Python venv, the
  baked probe/field data under `apps/forge/probes` (ignored by git, copied separately).
- **Tracked**: code, the learner's stage yamls (`apps/forge/python/configs`), `cluster.toml`, docs.

## 9. Data-flow diagram

```
                      human / forgectl                     other machines (cluster)
                            |                                   ^
                 console / SOAP / ssh+docker attach             | ClusterLink (7700) orders, PROGRESS
                            v                                   v
 +--------------------------------------------------------------------------------------------+
 | worldserver (C++)                                                                          |
 |  CLI thread --queue--> world thread  ForgeUpdateLoop -> World::Update                      |
 |                         |  OnWorldPrologue (clock, decision tick, apply tick)              |
 |                         |  MapMgr::Update --tasks--> map workers --------------------+      |
 |                         |        ^ join                 RunMapTick per map:          |      |
 |                         |        |                       apply actions / subtick      |      |
 |                         |        |                       Map::Update (game, controller)      |
 |                         |        |                       reward + observe + reset ----+      |
 |                         |  OnUpdate -> RemoteDecision                                 |      |
 |                         |      FinishCollect, SendStep ------ STEP ----+              |      |
 |                         |      ReceiveAny <---- ACT/MODE/WEIGHTS/... --+----------+  |      |
 |                         |  EnvPool arrays (Obs State Mask Rewards ...)  <-----------+--+      |
 |  sealed MySQL pools (no SQL after startup)                                  |          |      |
 +---------------------------------------------------------------|-------------|----------|-----+
                                          Unix socket / TCP (7701) |   HIP IPC (obs buffers) |
                                                                   v                         |
 +--------------------------------------------------------------------------------------------+
 | learner (Python, per rank)  ForgeEnv/ClusterEnv -> act (actor, mask) -> ACT                |
 |   RolloutBuffer -> GAE -> MAPPO update (maybe overlapped) -> weights                       |
 |   evaluation (MODE) -> ConvergenceController -> ADVANCE / continue                         |
 |   writes runs/<stage>/{metrics.csv, eval.jsonl, progress.json, latest.pt, best.pt, ...}    |
 +----------------------------|-------------------------------------|-------------------------+
        progress.json read by the sim (forge status)       weights between ranks (7702)
                                                              torch.distributed or async hub
```

## 10. Observed issues (orchestration layer)

- `AnimusForge.cpp:997-1002` (comment of `RunSeedable`) cites `stage19_duo_led` and `stage15_arena`, deleted v1 stages.
- `ForgeMain.cpp:9` header and the `ForgeUpdateLoop` docstring: "single process, never clustered"; see delta F-3, F-4.
- `AnimusForge.h:392-395`: the comment for `_turn`/`_nextTurn` ("Half-batch ... `_turn` is the group whose maps tick...")
  sits above `ClusterLink _cluster;` and the `_endedObs` comment ("SendStep's gather ... (protocol 14)") sits above
  `struct RankDevice`; both misplaced after edits.
- `EnvPool.h:194-195` (`CollectTiming`): `ResetScenarioNs` has two trailing comments, `ResetSeatsNs`
  none (swapped).
- `Scenario.cpp:25` comment refers to `.animus stage start`, a command of the separate mod-animus module.
- `ForgeConfig.cpp:747-756` (`LearnerConfigFor`): the comment speaks of per-class "gates" and "floors" (removed with
  pass gates) and `configs/<class>/<stage>.yaml`, a path no directory in the tree has.
- `ForgeConfig.h:60`: example uses `stage11_raids`.
- `cs_forge.cpp` help text for `forge clean archive` says "delete runs/_archive/"; the code deletes `<OutputDir>/archive`
  and the legacy `runs/_archive` (`ForgeCommands.cpp:51`, `cleanArchive`).
- `train.py:4` docstring uses `configs/stage4_duel.yaml`; `train.py` comment "stage4_duel sets 128"; `LearnerProcess.cpp`
  comment "stage8_duel's networks": v1 stage names.
- `stage.py:1-30` docstring says a class has converged when "all four" hold and then lists five.
- `progress.py` docstring cites `src/Console/Progress.cpp`; the file is `src/server/game/Animus/Console/Progress.cpp`.
- `LearnerProcess.cpp` `LearnerArgs`: `mappo.rank_sync` is only set when `DistWorld > 1` (a cluster); a purely local multi-rank
  run (`LearnerRanks > 1`, `DistWorld <= 1`) passes no `rank_sync` and the learner's yaml default applies. UNVERIFIED: that
  default's value.
- Apparent stale artefact: `mod-animus-amdl8.patch` patches block files (`SupportBlock`, `HostilesBlock`, ...) that no longer exist in this tree.

## 11. Reviewer notes

- The single most important invariant is who touches which memory when: map tasks touch only their own envs; the world
  thread touches shared managers only between the join and the next push; `ResetDefer` is the bridge. Any new code in
  a reset or an observation that calls a global manager must go through it.
- `_actionsPending`/`_applyTick` guarantee an action is applied in exactly one tick of exactly one group. Changing
  half-batch, `TicksPerDecision` or the prologue order breaks that silently (rewards scale per decision).
- Any change to `StepHeader`, `ActHeader`, SPEC or `PROTOCOL_VERSION` must be mirrored in `protocol.py` and bumps the
  fingerprint.
- The learner exits 0 for converged and for budget; the sim cannot tell them apart without reading `finished.json`.
- The lazy-import behaviour of the learner means changing files under a running learner can mix versions.
- `forge pause` does not reach workers; `forge resume` of a worker is only by a new START.
- Coverage of the rollout-graph code in the trimmed learner rests on GPU tests that are skipped on CPU ([tests.md](tests.md)).
