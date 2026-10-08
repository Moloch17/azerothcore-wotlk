# C++ runtime: configuration keys read by the runtime

Scope: every config key read by `ForgeConfig::Load` (`src/server/game/Animus/ForgeConfig.cpp:171`) and by the rest of the
runtime code in my area (`ForgeMain.cpp`, `Forge.cpp`, `AnimusForge.cpp`, `cs_forge.cpp`). Keys read by the curriculum
(`AnimusForge.Curriculum.*`, per-stage tuning) are in [cpp-tuning-keys.md](cpp-tuning-keys.md); the complete documented key list
with descriptions is `src/server/apps/worldserver/worldserver.conf.dist` (the `AnimusForge.*` block starts at line 4983,
`Forge.Playtest` at 105, `Forge.SealStrict` at 117); the operator view is [config-keys.md](config-keys.md). Entry point:
[cpp-runtime.md](cpp-runtime.md).

## Where the values come from

`ForgeMain.cpp:346-361`: `worldserver.conf` is loaded; then, if `<config dir>/modules/mod_animus_forge.conf` exists, it is merged in
full by `Config::LoadAdditionalFile(forgeConf, false)` after it, so a legacy module file **wins** over `worldserver.conf`.
`ForgeConfig::Load` runs in `Forge::OnStartup` (`AnimusForge.cpp:156`), once; there is no reload (`forge bench auto/apply`
patch the live `_config` for `Envs` and `LearnerTorchThreads` only, `ForgeCommands.cpp:1051-1054`). Relative path keys resolve
against the directory of the loaded `worldserver.conf` (`ConfigDir`, `ForgeConfig.cpp:57`), except `AnimusForge.Fast.OutputDir`
(relative to `OutputDir`).

A boolean parse or a missing key gives the default below. Where code clamps, the clamp is stated.

## Keys in `ForgeConfig::Load`

| Key | Default | Meaning and clamps | Line |
|---|---|---|---|
| `AnimusForge.Enable` | 1 | 0 turns every hook into a no-op; also read by `OutdoorPvPMgr.cpp:49` | 173 |
| `AnimusForge.Queue` | "" | comma list; empty = every stage | 175 |
| `AnimusForge.Queue.SkipFinished` | 1 | `forge start` without names skips advanced runs | 176 |
| `AnimusForge.Queue.LocalEpisodes` | 0 | episodes per scenario with a local policy | 177 |
| `AnimusForge.Classes` | "" | comma list; empty = every class; one class selects `configs/<class>/<stage>.yaml` when present (`LearnerConfigFor`) | 178 |
| `AnimusForge.Envs` | 64 | min 1; capped at `BotAccounts::MAX_ENVS` (1250) | 180 |
| `AnimusForge.ContinentReplicas` | 0 | 0 = fewest the 31 phase bits allow | 181 |
| `AnimusForge.Stage.<name>.Envs` | `AnimusForge.Envs` | per-stage env count; multiplied by the learner count in multi-GPU mode | 184-191 |
| `AnimusForge.DecisionMs` | 250 | min 1; reduced to a multiple of `TicksPerDecision`; made even under half-batch | 198, 208-227 |
| `AnimusForge.TicksPerDecision` | 1 | min 1; at most `DecisionMs` | 199, 202-207 |
| `AnimusForge.Stage.<name>.TicksPerDecision` | global value | per-stage split; dropped with an error if it does not divide `DecisionMs` | 230-248 |
| `AnimusForge.HalfBatch` | 0 | needs `TicksPerDecision` 1 (error otherwise); stages with a finer split run one group | 200 |
| `AnimusForge.ObserveAfterJoin` | 0 | read with logging off; documented in conf.dist (5626); a vision stage refuses it | 201 |
| `AnimusForge.EpisodeSeconds` | 60 | min 1 | 250 |
| `AnimusForge.Policy` | `remote` | `remote` = learner; anything else is a local policy name (only `random` exists) | 252 |
| `AnimusForge.ReportEpisodes` | 256 | min 1; episode-info mean window | 253 |
| `AnimusForge.ResetOnMapThreads` | 1 | rebuild ended episodes on the map's thread when the scenario allows | 254 |
| `AnimusForge.Socket` | `/tmp/animus-forge.sock` | Unix path, or `tcp://addr:port` | 259 |
| `AnimusForge.Learner.AutoStart` | 1 | start the learner as a child process | 262 |
| `AnimusForge.Learner.WorkDir` | "" | empty = compile-time `FORGE_PYTHON_DIR` (`apps/forge/python` of the build tree) | 264 |
| `AnimusForge.OutputDir` | "" | empty = the learner work directory; holds `runs/`, `layouts/`, `bench/` | 268 |
| `AnimusForge.Probe.Dir` | "" | empty = `<WorkDir parent>/probes` | 271 |
| `AnimusForge.Probe.CacheGrids` | 64 | min 1; layered-field grids kept in memory | 276 |
| `AnimusForge.Vision.Width` / `Height` | 128 / 64 | clamped 8..256 (error logged); not a multiple of the 16-column patch grid logs an error | 289-302 |
| `AnimusForge.Vision.FovH` / `FovV` | 120 / 60 | clamped 30..170 / 20..120 | 291-292 |
| `AnimusForge.Vision.Range` | 100 | clamped 10..500 yd | 293 |
| `AnimusForge.Vision.Zoom` | 6 | clamped 0..50 | 294 |
| `AnimusForge.Vision.Pitch` | -15 | clamped -80..80 | 295 |
| `AnimusForge.Vision.RenderSizes` | `32x16, 48x24, 64x32, 128x64:0.4` | sizes frames are cast at, with draw weights; invalid entries dropped with an error | 305-309 |
| `AnimusForge.Vision.AuditInterval` / `AuditSeats` | 300 / 4 | seconds (0 = never) 0..86400; seats 1..64 | 310-311 |
| `AnimusForge.Vision.EvalVideos` / `EvalVideoScale` | 8 / 4 | 0..64 episodes per evaluation; scale 1..8 | 312-313 |
| `AnimusForge.Map.MaxTiles` / `CoarseTiles` | 4096 / 0 | mental-map caps, clamped 1..65536 / 0..65536 | 317-319 |
| `AnimusForge.Map.KeepShare` / `AgeOffsetSeconds` | 0.5 / 600 | clamped 0..1 / 0..36000 | 320-321 |
| `AnimusForge.Memory.MaxEntities` | 64 | clamped 1..1024 | 324 |
| `AnimusForge.Learner.Python` | "" | empty = `<WorkDir>/.venv/bin/python` if it exists, else `python3`; a value with `/` is resolved against the config dir | 328-336 |
| `AnimusForge.Learner.Config` | "" | empty = `configs/<stage>.yaml` (or per class) in WorkDir | 339 |
| `AnimusForge.Learner.Args` | "" | whitespace-split extra learner args, appended last (they win) | 343 |
| `AnimusForge.Learner.LogFile` | "" | empty = `<LogsDir>/animus-learner.log`; `LogsDir` is the core key, **not** resolved here (relative if the core key is empty) | 347-355 |
| `AnimusForge.ModelDir` | "" | empty = `<WorkDir parent>/models` | 358 |
| `AnimusForge.Progress.Interval` | 0 | seconds between periodic reports; 0 = off | 361 |
| `AnimusForge.Fast.Envs` | 16 | min 1 | 363 |
| `AnimusForge.Fast.Budget` | 20000000 | env steps per stage, min 1000 | 364 |
| `AnimusForge.Fast.Queue` | "" | empty = every stage | 365 |
| `AnimusForge.Fast.OutputDir` | `fast` | relative to `OutputDir`; refused (replaced by `<OutputDir>/fast`) if it contains `OutputDir` | 367-381 |
| `AnimusForge.Fast.Learner.Overlay` | "" | empty = `<WorkDir>/configs/fast.yaml` | 384 |
| `AnimusForge.Fast.Learner.Args` | "" | extra learner args for fast runs | 389 |
| `AnimusForge.Learner.TorchThreads` | 0 | 0 = torch default | 393 |
| `AnimusForge.Learner.TrainDevice` / `RolloutDevice` | `auto` | "auto" or "" = decided by GPU mode / stage config | 404-405 |
| `AnimusForge.Learner.Cpus` | `auto` | CPU list for the learner; auto = cores the map update does not use | 406 |
| `AnimusForge.Cluster.Role` | `standalone` | `host`, `worker`, `standalone` (case-insensitive; other values -> standalone with an error) | 408-413 |
| `AnimusForge.Cluster.Host` | "" | `addr:port` of the host; a worker without it becomes standalone | 414, 425 |
| `AnimusForge.Cluster.ControlPort` / `DataPort` / `DistPort` | 7700 / 7701 / 7702 | cast to `uint16` without a range check | 415-418 |
| `AnimusForge.Cluster.Advertise` | "" | address the host should use for this worker's sim | 417 |
| `AnimusForge.Cluster.Sync` | `async` | `async` or `weights`, else `async` with an error | 419-424 |
| `AnimusForge.Cluster.Learner` | `auto` | worker runs its own learner: `0/false` off, `1/true` on, `auto` = on if a GPU counted | 484-492 |
| `AnimusForge.Bench.Scenario` | "" | stage to bench when `forge bench` has no argument | 432 |
| `AnimusForge.Bench.Policy` | `random` | local policy of the sim-only trials | 433 |
| `AnimusForge.Bench.Threads` / `Envs` | `auto` | list of positive numbers, or `auto` (grids worked out from physical cores and replicas, `AutoBenchGrids`) | 442-448 |
| `AnimusForge.Bench.MaxEnvs` | 512 | min 1 (header default 256 is stale) | 449 |
| `AnimusForge.Bench.WarmupTicks` / `MeasureTicks` | 128 / 384 | decisions, min 1 | 450-451 |
| `AnimusForge.Bench.LearnerWarmupTicks` / `LearnerMeasureTicks` | 384 / 768 | min 1 | 452-455 |
| `AnimusForge.Bench.MaxMemoryPercent` | 80 | clamped 1..100 | 456 |
| `AnimusForge.Bench.LearnerTop` | 2 | sim trials re-timed with the learner; 0 = none | 458 |
| `AnimusForge.Bench.LearnerTorchThreads` | `0, 8` | torch thread counts to try; 0 kept (= default) | 461 |
| `AnimusForge.Bench.AutoTune` | 0 | run `forge bench auto` at start on a CPU with no benchmark | 444 |
| `AnimusForge.SpawnPoint.MapId` | 560 | | 475 |
| `AnimusForge.SpawnPoint.X` / `Y` / `Z` / `O` | 2741.9 / 1315.2 / 14.0 / 2.96 | | 477-480 |
| `AnimusForge.Gpu.Observe` | 0 | load the device library at startup (remote policy only) | 483 |
| `AnimusForge.Gpu.Mode` | `auto` | `auto`, `single`, `multi`; counts GPUs by asking the learner's torch via `popen` | 564 |
| `AnimusForge.Gpu.Multi.Learners` | 0 | 0 = one per counted GPU; max 16 | 570 |
| `AnimusForge.Gpu.Single.Envs` / `Multi.Envs` | 0 | 0 = keep `Envs` (multiplied by learner count in multi) | 611-614 |
| `AnimusForge.Gpu.Single.Minibatches` / `Multi.Minibatches` | 0 | adds `--set mappo.minibatches=N` | 630 |
| `AnimusForge.Gpu.Single.LearnerArgs` / `Multi.LearnerArgs` | "" | appended after `Learner.Args` | 633 |

Not read from config: `ForgeConfig::Level` (always 0, see [cpp-runtime.md](cpp-runtime.md)), `ClusterSims`, `Dist*` (set per start by
`DealClusterLearners`).

## Keys read elsewhere in the runtime

| Key | Default | Read at | Notes |
|---|---|---|---|
| `Forge.Playtest` | 0 | `Forge.cpp:36` | once at startup; wall clock, listener, Warden, DB stays open |
| `Forge.SealStrict` | 1 | `ForgeMain.cpp:229` | strict seal also closes synchronous connections |
| `AnimusForge.DecisionMs` | 250 | `ForgeMain.cpp:276` | raw, min 1 |
| `AnimusForge.TicksPerDecision` | 1 | `ForgeMain.cpp:278` | raw, min 1 |
| `AnimusForge.HalfBatch` | 0 | `ForgeMain.cpp:281` | and `ticksPerDecision == 1` |
| `AnimusForge.Curriculum.*` | per key | `AnimusForge.cpp:93`, `StageSettings::TuningPrefix` | fingerprinted; see tuning-keys doc |
| `MapUpdate.Threads` | 1 | `AnimusForge.cpp:1702`, `ForgeConfig.cpp:497` | also written by `forge bench apply` |
| `RealmID` | 1 | `ForgeMain.cpp:183` | |
| `MinWorldUpdateTime` | 1 | `ForgeMain.cpp:246` | playtest loop only |
| `BindIP` / `Network.Threads` | `0.0.0.0` / 1 | `ForgeMain.cpp:434-436` | playtest listener only |
| `SOAP.Enabled` / `SOAP.IP` / `SOAP.Port` | 0 / `127.0.0.1` / 7878 | `ForgeMain.cpp:475-478` | |
| `Console.Enable` | 1 | `ForgeMain.cpp:489` | and stdin must be a tty |
| `LogsDir` | "" | `ForgeConfig.cpp:351` | for the learner log |

`ForgeMain` computes the world tick from the three raw keys (`tickMs = max(1, decisionMs / ticks / (halfBatch ? 2 : 1))`)
without `ForgeConfig`'s corrections (a `DecisionMs` that does not divide, an odd half-batch value). `Forge::StartCurrent`
overrides it per stage with `ForgeCore::SetTickMs(_runWorldTickMs)`. The only guard against disagreement is the one-time
error in `OnUpdate` (`AnimusForge.cpp:459-467`).

## Derived profiles

- `ForgeConfig::Stage(name)` -> `Animus::StageSettings` (`ForgeConfig.cpp:654`): envs (stage override capped), decision ms,
  episode seconds, report episodes, reset-on-map-threads, classes, spawn, `ContinentReplicas`, `TuningPrefix =
  "AnimusForge.Curriculum."`, layouts dir, `EventsLog = runs/<stage>/events.log`.
- `BenchProfile(envs, remote, torchThreads)` (`:686`): policy, envs, torch threads, `OutputDir = Bench.OutputDir`, learner
  args `eval.every_env_steps=1e12, eval.at_start=false, init_from=[], merge_from=[], distill.teachers="",
  total_env_steps=1e12, checkpoint_every=1000000, convergence.patience=0`, then `Learner.Args`.
- `FastProfile(budget)` (`:709`): policy remote, `Fast.Envs`, `ReportEpisodes <= 64`, output and model dir under
  `FastOutputDir`, learner args `--overlay <overlay> --set total_env_steps=<budget> --set convergence.patience=0`, then
  `Learner.Args`, then `Fast.Learner.Args`.
- `ApplyGpuMode` (`:562`): `learnerHere` = remote policy and (not a worker, or a learner-capable worker); GPUs are counted
  only then, keeping those with at least half the compute units of the largest; `MultiGpu` = learnerHere, not a worker, and
  mode `multi` or (`auto` and more than one GPU); `LearnerRanks` = learners (<= 16) in multi mode, else 1.

## Observed issues

- `ForgeConfig.h:37` says `mod_animus_forge.conf.dist` documents every key; no such file exists in the tree, the keys live in
  `worldserver.conf.dist`.
- `ForgeConfig.cpp:415-418`: ports are cast to `uint16` unchecked (`70000` wraps).
- `ForgeConfig.cpp:352`: `LearnerLogFile` is `LogsDir / "animus-learner.log"` unresolved. `ChildProcess::Start` creates the
  log's directory relative to the server's working directory, but opens the log after `chdir(WorkDir)` in the child
  (`ChildProcess.cpp:72-74`): with a relative `LogsDir` the two differ. UNVERIFIED whether the core makes `LogsDir`
  absolute; in the shipped layout it is configured.
- `ForgeConfig.cpp:136-168`: `DetectGpus` runs `popen("cd '<dir>' && '<python>' -c 'import torch ...'")` inside `Load`, with
  unescaped quotes; a path with a single quote breaks the command. Result cached for the process.
- `ForgeConfig.cpp:744`: `LearnerConfigFor`'s relative branch is unreachable for a configured value (`Load` already made it
  absolute).
- Two computations of the world tick (`ForgeMain.cpp:276-282` and `ForgeConfig.cpp:198-227`).

## Reviewer notes

- Any new key must be added to `worldserver.conf.dist`; `apps/forge/python/tests/test_conf_covers_tuning.py` checks the
  curriculum keys only (not these).
- Per-machine confs differ (cluster); only `AnimusForge.Curriculum.*` is fingerprinted, so `Envs`, `Stage.*.Envs`, `Vision.*`,
  `Map.*` and `Memory.*` can silently differ between machines. The host caps workers' envs and ticks per stage in START but
  does not check the camera or map settings; the learner's manifest check on `stage.json` would catch a vision size mismatch
  (UNVERIFIED).
