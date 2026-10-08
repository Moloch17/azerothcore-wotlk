# Configuration keys: `AnimusForge.*` and the other forge keys

Purpose and scope. Every `AnimusForge.*` key outside `AnimusForge.Curriculum.*` (those are in
[cpp-tuning-keys.md](cpp-tuning-keys.md)), the two `Forge.*` core keys, and the core keys the forge depends on. For
each key: group, default, range, reader, meaning, whether it must be the same on every cluster machine and whether the
cluster fingerprint covers it. The key table is generated (see "Regenerating the table"); everything else in this file
is hand-written from the code. Related: [config-yaml.md](config-yaml.md) (the learner's yaml), [tools-and-ops.md](
tools-and-ops.md) (conf tools, build, deploy), [01-forge-core-delta.md](01-forge-core-delta.md) (`Forge.*`),
[protocol.md](protocol.md) (what the fingerprint's protocol number is), [00-architecture.md](00-architecture.md).

## Map of the files in this area

| Path | Lines | Role |
|---|---|---|
| `src/server/apps/worldserver/worldserver.conf.dist` | 6755 | the config template; lines 4980-6000 are the FORGE section (`AnimusForge.*` keys, ~100 non-Curriculum plus 313 `Curriculum` keys) and lines 96-107 hold `Forge.SealStrict` |
| `src/server/game/Animus/ForgeConfig.h` | 318 | `ForgeConfig`: the typed settings struct, `BenchSettings`, `GpuMode`, `ClusterRole`, derived paths |
| `src/server/game/Animus/ForgeConfig.cpp` | 763 | `ForgeConfig::Load` (reads and validates every key), `ApplyGpuMode`, `AutoBenchGrids`, `BenchProfile`, `FastProfile`, `Stage`, `LearnerConfigFor` |
| `src/server/apps/worldserver/ForgeMain.cpp` | 520 | loads `worldserver.conf`, then the legacy module conf (lines 354-361); reads `DecisionMs`, `TicksPerDecision`, `HalfBatch` itself for the update loop (lines 276-281) |
| `src/server/game/Animus/AnimusForge.cpp` | 3087 | `ClusterFingerprint` (line 75), `DealClusterLearners`, `WorkerPlan` |
| `src/server/game/Animus/ForgeCommands.cpp` | 1304 | `ModuleConfigFile`, `WriteConfigValue` (lines 121-200): the only code that writes a conf |
| `apps/forge/tools/gen_config_reference.py` | 422 | generator of the key table below |
| `docs/forge/reference/config-keys.md` | this file | the document |
| per machine, not tracked: `env/dist/etc/worldserver.conf`, `env/dist/etc/modules/mod_animus_forge.conf` | 592 / 1228 on dev | the live settings (see "Per-machine layout") |

## How a key reaches the code

1. `ForgeMain.cpp:main` calls `sConfigMgr->Configure(<ConfigPath>/worldserver.conf, ...)` and `LoadAppConfigs()`
   (`ForgeMain.cpp:346-351`). This loads `worldserver.conf`; the template `worldserver.conf.dist` is never read by the
   server, only copied to `worldserver.conf` by `apps/docker/forge-worldserver.sh` when that file is missing
   (`forge-worldserver.sh:64-68`).
2. Then `modules/mod_animus_forge.conf` is read if it exists, with `LoadAdditionalFile(path, false)`
   (`ForgeMain.cpp:354-361`). It is read as a required (non-optional) file on purpose: as an optional file the config
   drops every key that the first file does not already define (`Config.cpp:240-246`, `AddKey`), which is every
   per-stage key (`AnimusForge.Stage.<name>.Envs`). Its values override `worldserver.conf`. The template comment says
   the same (conf.dist line 4983).
3. Parsing rules that matter (`Config.cpp:300-350`): `#` and `[` start a comment/section line; double quotes are
   deleted from values everywhere (so `"a b"` is `a b`); a repeated key inside one file is skipped (the first stays);
   a file with no assignment at all is "Empty file" and counts as a failure unless the policy says skip. A conf that
   was truncated to nothing therefore does not give defaults silently in every case, but deploy-gate.md and
   forgectl.md still require checking the line count after every conf write.
4. Environment variables win over both files: `GetOption` first looks for `AC_<KEY>` where the key's camel-case is
   split with underscores and upper-cased (`EnvVarForIniKey` and the cache lookup, `Config.cpp:375-445, 485-508`;
   `OverrideWithEnvVariablesIfAny`, 510-531, applied on reload). Example in
   the repository: `docker-compose.yml:142` sets `AC_ANIMUS_FORGE_OUTPUT_DIR` (key `AnimusForge.OutputDir`), so inside
   the containers the conf's `OutputDir` line is dead. (`GetKeysByString`, used for the `Stage.<name>.*` keys, walks the
   keys loaded from files, so an environment-only `Stage.<name>.Envs` is UNVERIFIED: check
   `ConfigMgr::GetKeysByString`.)
5. `ForgeConfig::Load` (`ForgeConfig.cpp:171-505` (the function body)) reads each key once, at
   `AnimusForge::Forge::OnStartup`
   (`AnimusForge.cpp`, `_config.Load()`). Nothing is re-read while running except by restart; there is no reload.
   `DecisionMs`, `TicksPerDecision` and `HalfBatch` are read a second time by `ForgeUpdateLoop`
   (`ForgeMain.cpp:276-281`) with the same defaults but without the cross-validation `ForgeConfig::Load` applies
   (so a `DecisionMs` that does not divide by `TicksPerDecision` is rounded in `ForgeConfig` but the update loop
   computes its own `tickMs` from the raw values: see Observed issues).
6. Relative path keys resolve against the directory of `worldserver.conf` (`ConfigDir`, `ForgeConfig.cpp:57-64`),
   except `Fast.OutputDir`, which resolves against `OutputDir`. Empty defaults resolve to the source tree: the learner
   work dir is `FORGE_PYTHON_DIR` baked in at configure time (`DefaultLearnerWorkDir`, `ForgeConfig.cpp:47-54`),
   models are `<workdir>/../models` (`ForgeConfig.cpp:358`).

## Key groups in prose

The table lists everything; this is what the groups are for.

- **General, queue**: `Enable` is also read by core code: `OutdoorPvPMgr.cpp:49` skips outdoor PvP when it is set, so
  `AnimusForge.Enable` changes core behaviour, not only the forge's. `Queue*` select what `forge start` trains by
  default; `Queue.LocalEpisodes` applies only to local (`random`) policies.
- **Environments**: `Envs` is the number of parallel envs (each its own instance map; at most `BotAccounts::MAX_ENVS` =
  100000 / (40 seats x 2 sessions) = 1250, `Bot/BotAccounts.h:35-42`). `DecisionMs` and `TicksPerDecision` define the
  two clocks (policy
  step and world tick); `Stage.<name>.TicksPerDecision` overrides the split per stage and is what makes every live
  stage tick at 50 ms (5 ticks of 50 ms in 250 ms). `HalfBatch` runs two env halves in turn; it needs
  `TicksPerDecision` 1 and a stage that has no per-stage tick, so with the live yaml-and-conf (all 12 stages tick 5) it
  never applies (`AnimusForge.cpp` run set-up, `_halfBatch = _config.HalvesTick() && _runTicks == 1`).
- **Policy**: `remote` (the Python learner over `Socket`) or `random`. `Policy` is not validated in `Load`;
  `Forge::KnowsPolicy` accepts only `random` for a local policy
  (`AnimusForge.cpp:2741-2744`); what `remote` vs an unknown string does at `forge start` is in cpp-runtime.md.
- **Camera vision, Map, Memory**: values outside range are clamped with a logged error (`ranged` lambda,
  `ForgeConfig.cpp:281-288`), not rejected. `Vision.Width/Height` set the size of the vision block: they are applied
  to the process (`Animus::Vision::Configure`) before any layout is built, and a model trained at one size does not fit
  another. Width must be a multiple of 16 and Height a multiple of Width/16; if not, an error is logged and the value is
  kept (the learner then refuses it).
- **Learner**: the command line the worldserver builds for the child learner process comes from these
  (`Learner/LearnerProcess.cpp`; see [py-learner.md](py-learner.md)). `Learner.Config` empty means
  `configs/<scenario>.yaml`, or `configs/<class>/<scenario>.yaml` when `Classes` names exactly one class and that file
  exists (`LearnerConfigFor`, `ForgeConfig.cpp:739-763`).
- **GPU mode**: `Gpu.Mode=auto` runs `python -c "import torch; ..."` through `popen` to count GPUs (`DetectGpus`,
  `ForgeConfig.cpp:136-166`), once per process, as a side effect of `ForgeConfig::Load`. A GPU counts if it has at least
  half the compute units of the largest. Multi mode multiplies `Envs` and each `Stage.<name>.Envs` by the number of
  learners unless `Gpu.Multi.Envs` is set.
- **Cluster**: see "Cluster keys" below.
- **Benchmark / Fast**: used only by `forge bench` and `forge fast` ([cpp-runtime.md](cpp-runtime.md)).
  `FastProfile` deliberately does not narrow classes or levels (`ForgeConfig.cpp:709-735`).

## Cluster keys and what must match

- Role comes from `Cluster.Role` (`standalone` default, `host`, `worker`); a worker with an empty `Cluster.Host` falls
  back to standalone with an error (`ForgeConfig.cpp:425-429`). Ports: 7700 control (host listens), 7701 data (worker
  sim listens), 7702 distributed-learner rendezvous (host).
- **Enforced match (the fingerprint).** `ClusterFingerprint` (`AnimusForge.cpp:75-105`) builds
  `src=<FORGE_SOURCE_HASH> protocol=<PROTOCOL_VERSION> curriculum=<FNV-1a hash of the effective
  AnimusForge.Curriculum.* tuning JSON> decision=<DecisionMs>/<TicksPerDecision>`. The host registers a worker only if
  the string is equal (`ClusterLink.cpp:293-310`); otherwise it logs `Cluster: refused the worker at ...`. From the
  non-Curriculum keys only `DecisionMs` and the global `TicksPerDecision` are in it.
  `FORGE_SOURCE_HASH` comes from the CMake configure step
  (`ForgeSourceHash.h`, `AnimusForge.cpp:63-67`; "unhashed" if the header is missing).
- **Not enforced, but must match** (deploy-gate.md step 6 table, confirmed against the code): `Vision.*`, `Map.*`,
  `Memory.MaxEntities`, `Classes`, `EpisodeSeconds`, `SpawnPoint.*`, `ContinentReplicas`, `HalfBatch`. Nothing compares
  them; a worker that differs trains a different episode distribution into the pool without any message.
- **Sent by the host at every START order** (so a worker's own value is ignored): the stage's world ticks per decision
  (`ticks=<n>`, `DealClusterLearners`, `AnimusForge.cpp:1262-1268`; applied by `WorkerPlan`, lines 1349-1361, which logs
  a warning once if its own value differed) and, when the host's conf has a `Stage.<name>.Envs` line for that stage, an
  env cap (`envs=<n>`); the worker runs `min(own, cap)`. This supersedes the deploy-gate.md remark that a worker
  without the `Stage.<stage>.TicksPerDecision` lines runs a 250 ms tick: the worker ticks as the host orders. Keep the
  lines equal anyway; a standalone start of the same worker would not have them.
- Per machine and free to differ: `Envs`, `Stage.<name>.Envs` (subject to the cap), `Cluster.*` ports and address,
  `Learner.*`, `Gpu.*`, `OutputDir`, `Socket`, `Bench.*`, `Fast.*`, `ModelDir`.

## Per-machine layout

The conf files are not tracked (`.gitignore`: `/env/dist/*`); `forgectl conf-sync` copies only the host's
`AnimusForge.Curriculum.*` keys to the workers ([forgectl.md](../forgectl.md)). The directory of one machine
(`env/dist/etc`): `worldserver.conf` (the core config, 592 active lines on the dev machine),
`modules/mod_animus_forge.conf`
(the forge keys; the legacy module-era file that still overrides), `*.conf.dist` copies, `authserver.conf`,
`dbimport.conf` (stock), many `mod_animus_forge.conf.bak-*` / `.before-*` backups, `animus-dashboard.auth` (not read;
may hold a secret), `backup-2026-09-15/`. `apps/docker/forge-worldserver.sh:64-68` copies any missing `.conf` from its
`.dist` on each container start and never overwrites. The dev machine's `modules/mod_animus_forge.conf.dist` is a
leftover of the module days (the repository has no such template; the current template is
`worldserver.conf.dist`).

What was read (the dev machine's files only; `UNVERIFIED: the worker confs on sarah, spencer, thomas and moloch were
not read (no ssh); the role split below comes from cluster.md`):

| Role | Keys that differ by role (from cluster.md and ForgeConfig) |
|---|---|
| host (sarah) | `Cluster.Role = "host"`, `Cluster.Host = ""`, `ControlPort`, `DistPort`, `Sync`; its learner uses `Learner.Cpus`; `MapUpdate.Threads/Cpus` in `worldserver.conf` |
| worker (spencer, thomas, moloch) | `Cluster.Role = "worker"`, `Cluster.Host = "<host>:7700"`, `DataPort`, `Advertise`, `Cluster.Learner`; its own `Envs`, `MapUpdate.*`, `Learner.Cpus` |

The dev machine's own file (read-only) has `Cluster.Role = "host"` and `Envs = 192`, `HalfBatch = 1`,
`Learner.Cpus = "16-31"`, `MapUpdate.Threads = 16`, `MapUpdate.Cpus = "0-15"` (`worldserver.conf`); it has no
`Stage.*.TicksPerDecision`, `Vision.*`, `Map.*`, `Memory.*`, `Gpu.*`, `ResetOnMapThreads`,
`Cluster.Learner/DistPort/Sync` lines (defaults apply), 13 per-stage `Envs` lines for archived stages, 239 Curriculum
keys. It is stale (see Observed issues). The key sets, computed by comparing the file to the template: 15 non-Curriculum
keys in the dev file that the template lacks, 45 in the template that the dev file lacks; 191 of the dev file's 239
Curriculum keys are not in the template and 265 of the template's 313 are not in the file.

## Related core keys the forge depends on

| Key | Where it is read | Role for the forge |
|---|---|---|
| `Forge.SealStrict` (default 1) | `ForgeMain.cpp:229` | after startup the databases are sealed; 1 closes every connection, so MySQL can be stopped |
| `MapUpdate.Threads` | `World/WorldConfig.cpp:574`; `AnimusForge.cpp:1702`; `ForgeConfig.cpp:497` | map task pool size; `forge bench apply` writes it |
| `MapUpdate.Cpus` | `Maps/MapUpdater.cpp:71` | CPU list for the map pool; `Learner.Cpus = auto` uses the CPUs this leaves free |
| `LogsDir` | `ForgeConfig.cpp` (learner log default `animus-learner.log` in it) | learner log location |
| `SOAP.Enabled/IP/Port`, `Console.Enable`, `Network.Threads`, `MinWorldUpdateTime`, `BindIP`, `RealmID` | `ForgeMain.cpp` | startup of the console/listener; dev `worldserver.conf` has `SOAP.Enabled = 1` on 127.0.0.1:7878, but `ForgeMain` replaces upstream `Main.cpp` ([cpp-runtime.md](cpp-runtime.md) for what it starts) |

## Writes to a conf

Only `forge bench apply` / `forge bench auto` write a conf (`ForgeCommands.cpp:1032-1041`): `MapUpdate.Threads` always
into
`worldserver.conf` (the file `sConfigMgr->GetFilename()`), and `AnimusForge.Envs` and `AnimusForge.Learner.TorchThreads`
into `ModuleConfigFile()`, which is `modules/mod_animus_forge.conf` when it exists, else `worldserver.conf`
(`ForgeCommands.cpp:121-128`). `WriteConfigValue` (`ForgeCommands.cpp:132-200`) rewrites lines matching the key,
appends if absent, copies the file to `<file>.before-bench` first (a single backup, overwritten each time), writes
`<file>.partial` and renames it over the file. A rename over a single-file bind mount would not be seen by a
container; the compose files mount the whole repository directory (`docker-compose.yml`,
`${DOCKER_VOL_ROOT:-.}:/azerothcore`), so it is
safe there. `forgectl` has its own conf writers ([tools-and-ops.md](tools-and-ops.md)).

## Regenerating the table

```
python3 apps/forge/tools/gen_config_reference.py            rewrite the table between the two marker lines
python3 apps/forge/tools/gen_config_reference.py --check    exit 1 if the table in this file is stale
python3 apps/forge/tools/gen_config_reference.py --stdout   print it only
```

It reads (never writes) the FORGE section of `worldserver.conf.dist`, every `.cpp`/`.h` under `src/` except `src/test`,
and `apps/forge/forgectl/*.py`, `apps/forge/tools/*.py`; it writes only the lines between the markers in this file.
Columns: "Template value" is the assignment in conf.dist; "Code default" is parsed from the `GetOption` or `ranged` call
(or the `STRUCT_DEFAULTS` table for values read from structs: camera, map, memory); "Range" is the template's
`(a to b)` or the `ranged` bounds or the `CODE_RULES` text; "Reader" is the first three `"AnimusForge.<key>"` literal
sites (a site marked "writes it" is `WriteConfigValue`). The two cluster columns come from the script's `SAME_RULES`
table, which must be edited by hand when the code changes (it names functions, not line numbers). Keys the code builds
at run time (`Gpu.Single|Multi.*`, `Stage.<name>.*`) are matched by the `DYNAMIC` table.

<!-- BEGIN GENERATED KEY TABLE (apps/forge/tools/gen_config_reference.py) -->

Generated from the FORGE section of `src/server/apps/worldserver/worldserver.conf.dist` and the
string literals in the sources; do not edit by hand
(run `python3 apps/forge/tools/gen_config_reference.py`). 101 keys.

| Group | Key | Template value | Code default | Range / validation | Reader | Same on every machine | In the cluster fingerprint | Meaning |
|---|---|---|---|---|---|---|---|---|
| General | AnimusForge.Enable | `1` | `true` | - | src/server/game/Animus/ForgeConfig.cpp:173, src/server/game/OutdoorPvP/OutdoorPvPMgr.cpp:49 | per machine | no | Enable the module. |
| General | AnimusForge.OutputDir | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:268 | per machine | no | Where training writes its data: runs/<scenario>/ (checkpoints, metrics, evaluation) and layouts/<scenario>/ (each model's layout manifest and the stage's stage.json). |
| Queue | AnimusForge.Queue | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:175 | per machine | no | Comma-separated scenarios `forge start` trains one after another when it is given none. |
| Queue | AnimusForge.Queue.SkipFinished | `1` | `true` | - | src/server/game/Animus/ForgeConfig.cpp:176 | per machine | no | `forge start` without scenarios leaves out queued scenarios whose run already finished and moved on (runs/<scenario>/finished.json in AnimusForge.OutputDir with "advanced": true), so it continues where training stopped instead of... |
| Queue | AnimusForge.Queue.LocalEpisodes | `0` | `0` | - | src/server/game/Animus/ForgeConfig.cpp:177 | per machine | no | With a local AnimusForge.Policy ("random", a scripted baseline), run this many episodes (over all envs) per queued scenario, then move to the next. |
| Queue | AnimusForge.Classes | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:178 | yes, by hand (what every episode is built from; deploy-gate.md step 6) | no | Comma-separated classes the curriculum stages play (their layouts). |
| Environments | AnimusForge.Envs | `64` | `64` | at least 1; capped at BotAccounts::MAX_ENVS, 1250 (also after the GPU mode scales it) | apps/forge/forgectl/confkeys.py:15 (tool, reads a conf file), apps/forge/forgectl/confkeys.py:58 (tool, reads a conf file), src/server/game/Animus/ForgeCommands.cpp:1033 (writes it) (+1) | per machine | no | Number of parallel environments. |
| Environments | AnimusForge.ContinentReplicas | `0` | `0` | - | src/server/game/Animus/ForgeConfig.cpp:181 | yes, by hand (what every episode is built from; deploy-gate.md step 6) | no | How many copies of the continent a continent stage spreads its envs over. |
| Environments | AnimusForge.Stage.<name>.Envs | (no line) | `AnimusForge.Envs` | - | src/server/game/Animus/ForgeConfig.cpp:187 | no: per machine; a host's line caps a worker's count for that stage (DealClusterLearners and WorkerPlan in AnimusForge.cpp) | no | a stage's own env count, where the default would not do. |
| Environments | AnimusForge.DecisionMs | `250` | `250` | at least 1; rounded down to a multiple of TicksPerDecision, with an error | src/server/apps/worldserver/ForgeMain.cpp:191, src/server/game/Animus/ForgeConfig.cpp:198 | yes, enforced (the fingerprint refuses a worker that differs) | yes: decision=<DecisionMs>/<TicksPerDecision> (ClusterFingerprint in AnimusForge.cpp) | Game time per agent decision, in milliseconds. |
| Environments | AnimusForge.TicksPerDecision | `1` | `1` | at least 1; capped at DecisionMs | apps/forge/forgectl/confkeys.py:56 (tool, reads a conf file), src/server/apps/worldserver/ForgeMain.cpp:193, src/server/game/Animus/ForgeConfig.cpp:199 | yes, enforced (the fingerprint refuses a worker that differs) | yes: decision=<DecisionMs>/<TicksPerDecision> (ClusterFingerprint in AnimusForge.cpp) | World updates per decision. |
| Environments | AnimusForge.Stage.<name>.TicksPerDecision | (no line) | - | - | src/server/game/Animus/ForgeConfig.cpp:233 | keep equal by hand; a worker runs the host's value anyway (the START order carries ticks=, WorkerPlan in AnimusForge.cpp) | no | One stage's own world updates per decision, where the default would not do: the movement stages want the finer world (smooth splines, the facing kept on a moving target between decisions, turns shown swung to a watching client) w... |
| Environments | AnimusForge.Stage.move1_controls.TicksPerDecision | `5` | - | - | src/server/game/Animus/ForgeConfig.cpp:233 | keep equal by hand; a worker runs the host's value anyway (the START order carries ticks=, WorkerPlan in AnimusForge.cpp) | no | One stage's own world updates per decision, where the default would not do: the movement stages want the finer world (smooth splines, the facing kept on a moving target between decisions, turns shown swung to a watching client) w... |
| Environments | AnimusForge.Stage.move2_seek.TicksPerDecision | `5` | - | - | src/server/game/Animus/ForgeConfig.cpp:233 | keep equal by hand; a worker runs the host's value anyway (the START order carries ticks=, WorkerPlan in AnimusForge.cpp) | no | One stage's own world updates per decision, where the default would not do: the movement stages want the finer world (smooth splines, the facing kept on a moving target between decisions, turns shown swung to a watching client) w... |
| Environments | AnimusForge.Stage.move3_interact.TicksPerDecision | `5` | - | - | src/server/game/Animus/ForgeConfig.cpp:233 | keep equal by hand; a worker runs the host's value anyway (the START order carries ticks=, WorkerPlan in AnimusForge.cpp) | no | One stage's own world updates per decision, where the default would not do: the movement stages want the finer world (smooth splines, the facing kept on a moving target between decisions, turns shown swung to a watching client) w... |
| Environments | AnimusForge.Stage.move4_follow.TicksPerDecision | `5` | - | - | src/server/game/Animus/ForgeConfig.cpp:233 | keep equal by hand; a worker runs the host's value anyway (the START order carries ticks=, WorkerPlan in AnimusForge.cpp) | no | One stage's own world updates per decision, where the default would not do: the movement stages want the finer world (smooth splines, the facing kept on a moving target between decisions, turns shown swung to a watching client) w... |
| Environments | AnimusForge.Stage.combat1_fight.TicksPerDecision | `5` | - | - | src/server/game/Animus/ForgeConfig.cpp:233 | keep equal by hand; a worker runs the host's value anyway (the START order carries ticks=, WorkerPlan in AnimusForge.cpp) | no | One stage's own world updates per decision, where the default would not do: the movement stages want the finer world (smooth splines, the facing kept on a moving target between decisions, turns shown swung to a watching client) w... |
| Environments | AnimusForge.Stage.combat2_packs.TicksPerDecision | `5` | - | - | src/server/game/Animus/ForgeConfig.cpp:233 | keep equal by hand; a worker runs the host's value anyway (the START order carries ticks=, WorkerPlan in AnimusForge.cpp) | no | One stage's own world updates per decision, where the default would not do: the movement stages want the finer world (smooth splines, the facing kept on a moving target between decisions, turns shown swung to a watching client) w... |
| Environments | AnimusForge.Stage.combat3_survive.TicksPerDecision | `5` | - | - | src/server/game/Animus/ForgeConfig.cpp:233 | keep equal by hand; a worker runs the host's value anyway (the START order carries ticks=, WorkerPlan in AnimusForge.cpp) | no | One stage's own world updates per decision, where the default would not do: the movement stages want the finer world (smooth splines, the facing kept on a moving target between decisions, turns shown swung to a watching client) w... |
| Environments | AnimusForge.Stage.group1_roles.TicksPerDecision | `5` | - | - | src/server/game/Animus/ForgeConfig.cpp:233 | keep equal by hand; a worker runs the host's value anyway (the START order carries ticks=, WorkerPlan in AnimusForge.cpp) | no | One stage's own world updates per decision, where the default would not do: the movement stages want the finer world (smooth splines, the facing kept on a moving target between decisions, turns shown swung to a watching client) w... |
| Environments | AnimusForge.Stage.group2_corridor.TicksPerDecision | `5` | - | - | src/server/game/Animus/ForgeConfig.cpp:233 | keep equal by hand; a worker runs the host's value anyway (the START order carries ticks=, WorkerPlan in AnimusForge.cpp) | no | One stage's own world updates per decision, where the default would not do: the movement stages want the finer world (smooth splines, the facing kept on a moving target between decisions, turns shown swung to a watching client) w... |
| Environments | AnimusForge.Stage.dungeon1_pulls.TicksPerDecision | `5` | - | - | src/server/game/Animus/ForgeConfig.cpp:233 | keep equal by hand; a worker runs the host's value anyway (the START order carries ticks=, WorkerPlan in AnimusForge.cpp) | no | One stage's own world updates per decision, where the default would not do: the movement stages want the finer world (smooth splines, the facing kept on a moving target between decisions, turns shown swung to a watching client) w... |
| Environments | AnimusForge.Stage.dungeon2_ragefire.TicksPerDecision | `5` | - | - | src/server/game/Animus/ForgeConfig.cpp:233 | keep equal by hand; a worker runs the host's value anyway (the START order carries ticks=, WorkerPlan in AnimusForge.cpp) | no | One stage's own world updates per decision, where the default would not do: the movement stages want the finer world (smooth splines, the facing kept on a moving target between decisions, turns shown swung to a watching client) w... |
| Environments | AnimusForge.Stage.dungeon3_deadmines.TicksPerDecision | `5` | - | - | src/server/game/Animus/ForgeConfig.cpp:233 | keep equal by hand; a worker runs the host's value anyway (the START order carries ticks=, WorkerPlan in AnimusForge.cpp) | no | One stage's own world updates per decision, where the default would not do: the movement stages want the finer world (smooth splines, the facing kept on a moving target between decisions, turns shown swung to a watching client) w... |
| Environments | AnimusForge.HalfBatch | `0` | `false` | needs TicksPerDecision 1 (error otherwise); an odd DecisionMs is lowered by 1 | apps/forge/forgectl/confkeys.py:57 (tool, reads a conf file), src/server/apps/worldserver/ForgeMain.cpp:196, src/server/game/Animus/ForgeConfig.cpp:200 | yes, by hand (what every episode is built from; deploy-gate.md step 6) | no | Split the envs into two halves whose maps tick in turn, so the learner decides one half while the other half's maps tick, instead of the sim and the learner taking turns. |
| Environments | AnimusForge.EpisodeSeconds | `60` | `60` | at least 1 | src/server/game/Animus/ForgeConfig.cpp:250 | yes, by hand (what every episode is built from; deploy-gate.md step 6) | no | Game-time length of an episode, in seconds. |
| Environments | AnimusForge.SpawnPoint.MapId | `560` | `560` | - | src/server/game/Animus/ForgeConfig.cpp:468 | yes, by hand (what every episode is built from; deploy-gate.md step 6) | no | Instanceable (dungeon) map and position where each env's bots start. |
| Environments | AnimusForge.SpawnPoint.X | `2741.9` | `2741.9f` | - | src/server/game/Animus/ForgeConfig.cpp:470 | yes, by hand (what every episode is built from; deploy-gate.md step 6) | no | Instanceable (dungeon) map and position where each env's bots start. |
| Environments | AnimusForge.SpawnPoint.Y | `1315.2` | `1315.2f` | - | src/server/game/Animus/ForgeConfig.cpp:471 | yes, by hand (what every episode is built from; deploy-gate.md step 6) | no | Instanceable (dungeon) map and position where each env's bots start. |
| Environments | AnimusForge.SpawnPoint.Z | `14.0` | `14.0f` | - | src/server/game/Animus/ForgeConfig.cpp:472 | yes, by hand (what every episode is built from; deploy-gate.md step 6) | no | Instanceable (dungeon) map and position where each env's bots start. |
| Environments | AnimusForge.SpawnPoint.O | `2.96` | `2.96f` | - | src/server/game/Animus/ForgeConfig.cpp:473 | yes, by hand (what every episode is built from; deploy-gate.md step 6) | no | Instanceable (dungeon) map and position where each env's bots start. |
| Policy | AnimusForge.Policy | `"remote"` | `"remote"` | - | src/server/game/Animus/ForgeConfig.cpp:252 | per machine | no | Who chooses actions. |
| Policy | AnimusForge.ReportEpisodes | `256` | `256` | at least 1 | src/server/game/Animus/ForgeConfig.cpp:253 | per machine | no | Mean episode statistics are taken over this many completed episodes (all policies, including remote). |
| Policy | AnimusForge.Socket | `"/tmp/animus-forge.sock"` | `"/tmp/animus-forge.sock"` | - | src/server/game/Animus/ForgeConfig.cpp:259 | per machine | no | Unix domain socket path the remote policy listens on, relative to the directory of worldserver.conf or absolute. |
| Episode Resets | AnimusForge.ResetOnMapThreads | `1` | `true` | - | src/server/game/Animus/ForgeConfig.cpp:254 | per machine | no | Rebuild each ended episode on the thread that updates its map, right after that map's tick, instead of on the world thread after every map has finished. |
| Camera Vision | AnimusForge.Vision.Width | `128` | `128` | 8.0 to 256.0 | src/server/game/Animus/ForgeConfig.cpp:282 | yes (a mismatch is caught: the learner checks the SPEC image byte count; deploy-gate.md step 6) | no | Image columns of the vision block's camera (stages with a vision block, move1_controls): the canonical image, the one the learner's network reads. |
| Camera Vision | AnimusForge.Vision.Height | `64` | `64` | 8.0 to 256.0 | src/server/game/Animus/ForgeConfig.cpp:283 | yes (a mismatch is caught: the learner checks the SPEC image byte count; deploy-gate.md step 6) | no | Image rows of the camera's canonical image. |
| Camera Vision | AnimusForge.Vision.RenderSizes | `"32x16, 48x24, 64x32, 128x64:0.4"` | `"32x16, 48x24, 64x32, 128x64:0.4"` | entries that do not parse or exceed Width x Height are dropped with an error | src/server/game/Animus/ForgeConfig.cpp:299 | yes, by hand (nothing compares it; deploy-gate.md step 6) | no | The sizes frames are actually cast at, "WxH" or "WxH:weight" separated by commas, each at most Width x Height: the same field of view with fewer, wider rays, scaled up into the canonical image by nearest pixel. |
| Camera Vision | AnimusForge.Vision.FovH | `120` | `120` | 30.0 to 170.0 | src/server/game/Animus/ForgeConfig.cpp:284 | yes, by hand (nothing compares it; deploy-gate.md step 6) | no | Horizontal field of view, degrees. |
| Camera Vision | AnimusForge.Vision.FovV | `60` | `60` | 20.0 to 120.0 | src/server/game/Animus/ForgeConfig.cpp:285 | yes, by hand (nothing compares it; deploy-gate.md step 6) | no | Vertical field of view, degrees. |
| Camera Vision | AnimusForge.Vision.Range | `100` | `100` | 10.0 to 500.0 | src/server/game/Animus/ForgeConfig.cpp:286 | yes, by hand (nothing compares it; deploy-gate.md step 6) | no | Yards around the camera within which units are seen. |
| Camera Vision | AnimusForge.Vision.Zoom | `6` | `6` | 0.0 to 50.0 | src/server/game/Animus/ForgeConfig.cpp:287 | yes, by hand (nothing compares it; deploy-gate.md step 6) | no | Yards from the pivot (the head) back to the camera, before the boom pulls in at a wall. 0 sees from the eyes. |
| Camera Vision | AnimusForge.Vision.Pitch | `-15` | `-15` | -80.0 to 80.0 | src/server/game/Animus/ForgeConfig.cpp:288 | yes, by hand (nothing compares it; deploy-gate.md step 6) | no | The camera's pitch, degrees, + up: the default looks slightly down. |
| Camera Vision | AnimusForge.Vision.AuditInterval | `300` | `300.0f` | 0.0 to 86400.0 | src/server/game/Animus/ForgeConfig.cpp:303 | no: a local audit of the camera | no | Every this many seconds of real time while a stage with a camera trains or runs, save AuditSeats seats' frames, exactly as the learner gets them, to runs/<scenario>/camera/: one PNG a frame (depth, kind, height over the feet and... |
| Camera Vision | AnimusForge.Vision.AuditSeats | `4` | `4.0f` | 1.0 to 64.0 | src/server/game/Animus/ForgeConfig.cpp:304 | no: a local audit of the camera | no | Frames saved at each audit. |
| Camera Vision | AnimusForge.Vision.EvalVideos | `8` | `8.0f` | 0.0 to 64.0 | src/server/game/Animus/ForgeConfig.cpp:305 | yes, by hand (nothing compares it; deploy-gate.md step 6) | no | Episodes of every evaluation filmed as the seat saw them, for watching later: one frame a decision (the composite picture, with the mini-map inset where the stage has a map), written as an animated PNG (a browser plays it) with a... |
| Camera Vision | AnimusForge.Vision.EvalVideoScale | `4` | `4.0f` | 1.0 to 8.0 | src/server/game/Animus/ForgeConfig.cpp:306 | yes, by hand (nothing compares it; deploy-gate.md step 6) | no | Each camera pixel drawn this many times as wide and high in the videos (nearest pixel): 4 makes a 128 x 64 camera 512 x 256. |
| Camera Vision | AnimusForge.Map.MaxTiles | `4096` | `4096` | 1.0 to 65536.0 | src/server/game/Animus/ForgeConfig.cpp:310 | yes, by hand (nothing compares it; deploy-gate.md step 6) | no | The mental map (a stage with a map block, perception-goals REDESIGN §3): the most 32 x 32-yard tiles a seat's map keeps, the least recently written going first. |
| Camera Vision | AnimusForge.Map.CoarseTiles | `0` | `0` | 0.0 to 65536.0 | src/server/game/Animus/ForgeConfig.cpp:311 | yes, by hand (nothing compares it; deploy-gate.md step 6) | no | Coarse tiles (8-yard cells, 256 yards a tile) an evicted fine tile is folded into rather than forgotten, read where no fine tile is kept. 0 forgets it (training). |
| Camera Vision | AnimusForge.Map.KeepShare | `0.5` | `0.5` | 0.0 to 1.0 | src/server/game/Animus/ForgeConfig.cpp:313 | yes, by hand (nothing compares it; deploy-gate.md step 6) | no | The share of a training seat's resets on the same instance that keep its map (stale memory, as a shipped bot has); the others start it empty. |
| Camera Vision | AnimusForge.Map.AgeOffsetSeconds | `600` | `600` | 0.0 to 36000.0 | src/server/game/Animus/ForgeConfig.cpp:314 | yes, by hand (nothing compares it; deploy-gate.md step 6) | no | A kept map is this much older at most, drawn uniformly from 0 at each reset that keeps it: every look and entity in it ages by the draw. |
| Camera Vision | AnimusForge.Memory.MaxEntities | `64` | `64` | 1 to 1024 | src/server/game/Animus/ForgeConfig.cpp:317 | yes, by hand (nothing compares it; deploy-gate.md step 6) | no | Entity memory (a stage with a sight block, dungeon-curriculum I2): the most entities a seat remembers -- what each was, where and how it was last seen -- the least recently seen going first. |
| Learner | AnimusForge.Learner.AutoStart | `1` | `true` | - | src/server/game/Animus/ForgeConfig.cpp:262 | per machine | no | With AnimusForge.Policy = "remote", start the Python learner automatically whenever a scenario of `forge start` or `forge resume` is ready: <Python> -u -m animus.train --config <Config> --socket <AnimusForge.Socket> --run-name <s... |
| Learner | AnimusForge.Learner.WorkDir | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:264 | per machine | no | Directory the learner runs in (the module's python/ directory). |
| Learner | AnimusForge.Learner.Python | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:321 | per machine | no | Python interpreter for the learner: a name found on PATH, or a path (relative to the directory of worldserver.conf, or absolute). |
| Learner | AnimusForge.Learner.Config | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:332 | per machine | no | Learner config for every scenario, relative to the directory of worldserver.conf or absolute. |
| Learner | AnimusForge.Learner.Args | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:336 | per machine | no | Extra learner arguments appended to every learner command line, split on spaces (no quoting). |
| Learner | AnimusForge.Learner.TorchThreads | `0` | `0` | - | src/server/game/Animus/ForgeCommands.cpp:1035 (writes it), src/server/game/Animus/ForgeConfig.cpp:386 | per machine | no | CPU threads the learner's torch may use (--set torch_threads). |
| Learner | AnimusForge.Learner.TrainDevice | `"auto"` | `"auto"` | - | src/server/game/Animus/ForgeConfig.cpp:397 | per machine | no | Where the learner updates its networks (TrainDevice) and where it runs the envs' action inference (RolloutDevice): "cuda:0", "cuda:1", "cpu" ("cuda" means the ROCm or CUDA device alike). "auto" leaves it to the GPU mode: each lea... |
| Learner | AnimusForge.Learner.RolloutDevice | `"auto"` | `"auto"` | - | src/server/game/Animus/ForgeConfig.cpp:398 | per machine | no | Where the learner updates its networks (TrainDevice) and where it runs the envs' action inference (RolloutDevice): "cuda:0", "cuda:1", "cpu" ("cuda" means the ROCm or CUDA device alike). "auto" leaves it to the GPU mode: each lea... |
| Learner | AnimusForge.Learner.Cpus | `"auto"` | `"auto"` | - | apps/forge/forgectl/confkeys.py:15 (tool, reads a conf file), apps/forge/forgectl/confkeys.py:59 (tool, reads a conf file), src/server/game/Animus/ForgeConfig.cpp:399 | per machine | no | The CPUs the learner runs on, as a list like "8-15,24-31"; several learners (multi mode) share them out by whole physical cores. "auto" gives it every core the map update (MapUpdate.Cpus) does not use. |
| Gpu Mode | AnimusForge.Gpu.Observe | `0` | `false` | - | src/server/game/Animus/ForgeConfig.cpp:476 | per machine | no | Load the forge's device library (libforge-gpu.so, beside the worldserver, built where hipcc is) on the learner's own HIP runtime, so observations can be written straight into GPU memory the learner reads. |
| Gpu Mode | AnimusForge.ObserveAfterJoin | `0` | `false` | - | src/server/game/Animus/ForgeConfig.cpp:201 | per machine | no | Hold each map's observation back until every map task of the update has joined, then run them as map work again: the barrier a device kernel over every seat would need. |
| Gpu Mode | AnimusForge.Gpu.Mode | `"auto"` | `"auto"` | auto, single, multi (anything else: error, auto) | src/server/game/Animus/ForgeConfig.cpp:557 | per machine | no | auto: count the GPUs the learner's torch sees (ROCm or CUDA) with at least half the compute units of the largest -- an integrated GPU beside a card does not count -- and run multi mode with two or more, single mode otherwise. |
| Gpu Mode | AnimusForge.Gpu.Single.Envs | `0` | `0` | - | src/server/game/Animus/ForgeConfig.cpp:605 | per machine | no | Envs in that mode, in place of AnimusForge.Envs. 0: single mode uses AnimusForge.Envs; multi mode uses AnimusForge.Envs (and each AnimusForge.Stage.<name>.Envs) times the learners, so every GPU gets the batch one GPU would. |
| Gpu Mode | AnimusForge.Gpu.Multi.Envs | `0` | `0` | - | src/server/game/Animus/ForgeConfig.cpp:605 | per machine | no | Envs in that mode, in place of AnimusForge.Envs. 0: single mode uses AnimusForge.Envs; multi mode uses AnimusForge.Envs (and each AnimusForge.Stage.<name>.Envs) times the learners, so every GPU gets the batch one GPU would. |
| Gpu Mode | AnimusForge.Gpu.Single.Minibatches | `0` | `0` | - | src/server/game/Animus/ForgeConfig.cpp:623 | per machine | no | mappo.minibatches in that mode (0 = the stage config's). |
| Gpu Mode | AnimusForge.Gpu.Multi.Minibatches | `0` | `0` | - | src/server/game/Animus/ForgeConfig.cpp:623 | per machine | no | mappo.minibatches in that mode (0 = the stage config's). |
| Gpu Mode | AnimusForge.Gpu.Single.LearnerArgs | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:626 | per machine | no | Extra learner arguments in that mode, after AnimusForge.Learner.Args (so a --set here wins), e.g. "--set mappo.lr=0.0005". |
| Gpu Mode | AnimusForge.Gpu.Multi.LearnerArgs | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:626 | per machine | no | Extra learner arguments in that mode, after AnimusForge.Learner.Args (so a --set here wins), e.g. "--set mappo.lr=0.0005". |
| Gpu Mode | AnimusForge.Gpu.Multi.Learners | `0` | `0` | capped at 16 | src/server/game/Animus/ForgeConfig.cpp:563 | per machine | no | Learners in multi mode (1-16). 0: one per GPU counted. |
| Cluster | AnimusForge.Cluster.Role | `"standalone"` | `"standalone"` | standalone, host or worker (anything else: error, standalone); a worker with an empty Host falls back to standalone | apps/forge/forgectl/confsync.py:85 (tool, reads a conf file), src/server/game/Animus/ForgeConfig.cpp:401 | per machine (host or worker) | no | standalone (one machine, as always), host or worker. |
| Cluster | AnimusForge.Cluster.Host | `""` | `""` | - | apps/forge/forgectl/confsync.py:85 (tool, reads a conf file), src/server/game/Animus/ForgeConfig.cpp:407 | per machine (worker keys) | no | the host's control address, "address:port" (its ControlPort). |
| Cluster | AnimusForge.Cluster.ControlPort | `7700` | `7700` | - | src/server/game/Animus/ForgeConfig.cpp:408 | host key: every machine reads it, only a host uses it | no | the TCP port workers register on and take orders from. |
| Cluster | AnimusForge.Cluster.DataPort | `7701` | `7701` | - | src/server/game/Animus/ForgeConfig.cpp:409 | per machine (worker keys) | no | the TCP port its sim listens on for the host's learner. |
| Cluster | AnimusForge.Cluster.Advertise | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:410 | per machine (worker keys) | no | the address the host's learner should reach it at, when the address its registration comes from is not it (NAT, several interfaces). |
| Cluster | AnimusForge.Cluster.Learner | `"auto"` | `"auto"` | auto, 0/false, 1/true | src/server/game/Animus/ForgeConfig.cpp:477 | per machine (worker keys) | no | run a learner of its own on this machine's GPU, training on this sim as one rank of the host's run: the host gives it a rank when it starts a scenario, and every learner's networks are averaged once an update (mappo.rank_sync = w... |
| Cluster | AnimusForge.Cluster.DistPort | `7702` | `7702` | - | src/server/game/Animus/ForgeConfig.cpp:411 | host key: every machine reads it, only a host uses it | no | the TCP port every machine's learners meet on (torch.distributed). |
| Cluster | AnimusForge.Cluster.Sync | `"async"` | `"async"` | async or weights (anything else: error, async) | src/server/game/Animus/ForgeConfig.cpp:412 | host key: every machine reads it, only a host uses it | no | how the machines' learners (AnimusForge.Cluster.Learner) share what they learn. async: each trains at its own pace on its own sim and trades its networks with the host's learner in the background (animus.async_sync); the host's l... |
| Cluster | AnimusForge.Learner.LogFile | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:340 | per machine | no | File the learner's stdout and stderr are appended to, relative to the directory of worldserver.conf or absolute. |
| Benchmark | AnimusForge.Bench.Scenario | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:425 | per machine | no | Scenario the trials run, when `forge bench` is given none. |
| Benchmark | AnimusForge.Bench.Policy | `"random"` | `"random"` | - | src/server/game/Animus/ForgeConfig.cpp:426 | per machine | no | Local policy the sim-only trials play ("random", the only one: there are no scripted policies). |
| Benchmark | AnimusForge.Bench.Threads | `"auto"` | `"auto"` | a list of positive numbers, or auto | src/server/game/Animus/ForgeConfig.cpp:435, src/server/game/Animus/ForgeConfig.cpp:439 | per machine | no | MapUpdate.Threads values to try, comma-separated. |
| Benchmark | AnimusForge.Bench.Envs | `"auto"` | `"auto"` | a list of positive numbers, or auto | src/server/game/Animus/ForgeConfig.cpp:436, src/server/game/Animus/ForgeConfig.cpp:441 | per machine | no | AnimusForge.Envs values to try, comma-separated. |
| Benchmark | AnimusForge.Bench.MaxEnvs | `512` | `512` | - | src/server/game/Animus/ForgeConfig.cpp:442 | per machine | no | Never try more envs than this, whatever AnimusForge.Bench.Envs says. |
| Benchmark | AnimusForge.Bench.WarmupTicks | `128` | `128` | - | src/server/game/Animus/ForgeConfig.cpp:443 | per machine | no | Decisions a sim-only trial runs before it is timed, so the first episode (which every env starts together) and the cold caches are behind it. |
| Benchmark | AnimusForge.Bench.MeasureTicks | `384` | `384` | - | src/server/game/Animus/ForgeConfig.cpp:444 | per machine | no | Decisions timed per sim-only trial. |
| Benchmark | AnimusForge.Bench.MaxMemoryPercent | `80` | `80` | clamped to 1..100 | src/server/game/Animus/ForgeConfig.cpp:450 | per machine | no | Once the machine's memory is this used after a trial, the bigger env counts of that thread count are skipped instead of being built. |
| Benchmark | AnimusForge.Bench.LearnerTop | `2` | `2` | - | src/server/game/Animus/ForgeConfig.cpp:451 | per machine | no | How many of the fastest sim trials run again with the real learner, which is what training actually costs. |
| Benchmark | AnimusForge.Bench.LearnerWarmupTicks | `384` | `384` | - | src/server/game/Animus/ForgeConfig.cpp:446 | per machine | no | The learner phase's own warm-up, which also has to cover the learner starting up and its first updates (one update per rollout_length decisions, 128 by default). |
| Benchmark | AnimusForge.Bench.LearnerMeasureTicks | `768` | `768` | - | src/server/game/Animus/ForgeConfig.cpp:448 | per machine | no | Decisions timed per learner trial. |
| Benchmark | AnimusForge.Bench.LearnerTorchThreads | `"0, 8"` | `"0, 8"` | - | src/server/game/Animus/ForgeConfig.cpp:454 | per machine | no | AnimusForge.Learner.TorchThreads values to try in the learner trials, comma-separated (0 = torch's own default). |
| Benchmark | AnimusForge.Bench.AutoTune | `0` | `false` | - | src/server/game/Animus/ForgeConfig.cpp:437 | per machine | no | On a machine with no benchmark of its own CPU in the bench output directory (a fresh copy of the project, or a copy from another machine), run `forge bench auto` when the worldserver starts: the benchmark above, then its winner w... |
| Fast Test Run | AnimusForge.Fast.Queue | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:358 | per machine | no | Comma-separated scenarios `forge fast` trains when given none, in order, each again from scratch (nothing is skipped because an earlier fast run of it finished). |
| Fast Test Run | AnimusForge.Fast.Envs | `32` | `16` | at least 1 | src/server/game/Animus/ForgeConfig.cpp:356 | per machine | no | Parallel envs of a fast run. |
| Fast Test Run | AnimusForge.Fast.Budget | `20000000` | `20000000` | at least 1000 | src/server/game/Animus/ForgeConfig.cpp:357 | per machine | no | Env steps each stage of a fast run trains for before the next one starts. |
| Fast Test Run | AnimusForge.Fast.OutputDir | `"fast"` | `"fast"` | must not contain AnimusForge.OutputDir (error; falls back to <OutputDir>/fast) | src/server/game/Animus/ForgeConfig.cpp:360 | per machine | no | Where a fast run's runs/, layouts/ and exported models/ go; relative to AnimusForge.OutputDir. |
| Fast Test Run | AnimusForge.Fast.Learner.Overlay | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:377 | per machine | no | Learner config merged over every stage's config in a fast run (--overlay), relative to the directory of worldserver.conf or absolute. |
| Fast Test Run | AnimusForge.Fast.Learner.Args | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:382 | per machine | no | Extra learner arguments for fast runs only, after AnimusForge.Learner.Args. |
| Console | AnimusForge.ModelDir | `""` | `""` | - | src/server/game/Animus/ForgeConfig.cpp:351, src/server/scripts/Commands/cs_forge.cpp:179 | per machine | no | Folder `forge export` writes .amdl models (and their layout manifests) to, relative to the directory of worldserver.conf or absolute. |
| Console | AnimusForge.Progress.Interval | `0` | `0` | - | src/server/game/Animus/ForgeConfig.cpp:354 | per machine | no | While a scenario runs, also log the progress report every this many wall-clock seconds: env steps, step rate, ETAs (step limit, earliest convergence, the whole plan), evaluation scores against the baseline, restarts, losses and w... |

Template value differs from the code default (numeric keys, the template line is what a copied conf.dist starts with):

| Key | Template | Code default | Reader |
|---|---|---|---|
| AnimusForge.Fast.Envs | `32` | `16` | src/server/game/Animus/ForgeConfig.cpp:356 |

Keys documented in the template with no reader found by the literal scan: none.

<!-- END GENERATED KEY TABLE -->

## Observed issues

- `ForgeMain.cpp:276-281` recomputes the tick length from the raw `DecisionMs` / `TicksPerDecision` without the rounding
  `ForgeConfig::Load` applies (`ForgeConfig.cpp:201-225`); with a `DecisionMs` that does not divide, the update loop
  and the module could disagree (the module logs a tick-mismatch warning for this, `AnimusForge.cpp`
  `_tickMismatchLogged`).
- `ForgeConfig.h` `BenchSettings::MaxEnvs = 256` but `Load` defaults to 512 (`ForgeConfig.cpp:449`) and the template
  says
  512. `FastEnvs` header and code default are 16 but the template assigns 32 (`worldserver.conf.dist` `Fast.Envs = 32`):
  a copied template trains fast runs at 32 envs, the code alone at 16.
- `ForgeConfig::LearnerConfigFor` tests `named.is_relative()` (`ForgeConfig.cpp:742-746`) but `Load` already resolved
  `LearnerConfig` to an absolute path (lines 339-340), so the relative branch is dead.
- `ForgeConfig::Load` runs `python -c "import torch ..."` via `popen` (`DetectGpus`, `ForgeConfig.cpp:136-166`): config
  loading has a process-spawning side effect, once per process, and needs the learner venv to exist.
- Template prose out of date: `Queue.LocalEpisodes` mentions "a scripted baseline" (none exist); the FAST section
  intro says "a few classes at one level" (`FastProfile` explicitly does not narrow, `ForgeConfig.cpp:709-735`); the GPU
  and Cluster intros say a worker runs no learner while `Cluster.Learner` exists; `Cluster.Learner` text says
  `rank_sync = weights` while `Cluster.Sync` defaults to `async`; `ObserveAfterJoin` and `Gpu.Observe` cite measurements
  on the deleted `stage4_duel`; `Bench.Scenario` text mentions `stage4_duel` as former default.
- `ObserveAfterJoin` sits in the "GPU MODE" group of the template but is a map-update switch, and `Learner.LogFile` sits
  in the "CLUSTER" group: grouping in the template is by position, not by subject.
- The dev machine's conf (`env/dist/etc/modules/mod_animus_forge.conf`, read-only) is stale: `Cluster.Role = "host"`
  although cluster.md says the host is sarah and dev is not in the cluster (a worldserver started on dev would listen
  on 7700 as a second host); `HalfBatch = 1` disagrees with the template; it has `AnimusForge.MoveRevision` (no reader
  anywhere in `src/` or `apps/`), 13 `Stage.stageN_*.Envs` lines and two
  `Stage.stage1_move/stage2_travel.TicksPerDecision`
  lines for archived stages, and `Bench.Scenario = "stage4_duel"` (an archived stage; a bench would fail). It lacks
  every line listed above, so on dev the movement stages would run the global tick 1. Of its 239 Curriculum keys, 191
  are not in the template. cluster.md and forgectl.md quote "239 keys": that is a count, not proof the keys are current
  (the fingerprint hashes effective values, so stale unread keys are harmless to it). The cluster machines were not
  read.
- `AnimusForge.Enable` is read in core code (`OutdoorPvPMgr.cpp:49`) with a third argument `false` (do not warn), while
  the forge
  reads it with the default warning: cross-reference for [01-forge-core-delta.md](01-forge-core-delta.md).
- `conf.dist` documents `Stage.<name>.Envs` with no assignment line and states "None are set"; the dev conf sets 13
  (all archived).

## Reviewer notes

- Is the fingerprint's omission of `Vision.*`, `Map.*`, `Classes`, `EpisodeSeconds`, `SpawnPoint.*` deliberate? Only a
  deploy-gate checklist protects them today.
- Should `ForgeUpdateLoop` take its three keys from `ForgeConfig` instead of re-reading them?
- `Policy` is stored as a string and compared in several places (`IsRemote()`, `KnowsPolicy`); an enum would remove the
  string compares.
- Moving the FORGE keys out of `worldserver.conf.dist` into a separate template would remove the legacy-module-conf
  special case in `ForgeMain.cpp:354-361` and the `ModuleConfigFile()` branch in `ForgeCommands.cpp:121-128`.
