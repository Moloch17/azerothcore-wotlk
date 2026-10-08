# C++ runtime: the console, the report and `progress.json` as the sim reads it

Scope: `src/server/scripts/Commands/cs_forge.cpp` (command table), `A/ForgeCommands.cpp` (`Forge::Command*`),
`A/Console/Progress.{h,cpp}`, `A/Console/TextTable.{h,cpp}` (`A/` = `src/server/game/Animus/`). Entry point:
[cpp-runtime.md](cpp-runtime.md). Operator-level use of the commands is in [forgectl.md](../forgectl.md) and
[tools-and-ops.md](tools-and-ops.md); the learner's writer of `progress.json` is in [py-learner.md](py-learner.md); metric
names in [metrics.md](metrics.md).

## How a command reaches the code

The console thread (`CliRunnable.cpp:231`; only started when `Console.Enable` is on **and** stdin is a terminal,
`ForgeMain.cpp:488-495`) or the SOAP thread (when `SOAP.Enabled`, `ForgeMain.cpp:475`) queues a `CliCommandHolder`;
`World::ProcessCliCommands` (`World.cpp:1505`) runs it on the world thread, between the map update and `Forge::OnUpdate`
(`World.cpp:1258`), or from `Forge::Pump()` while the world thread waits for the learner or sits paused. All commands are
`SEC_ADMINISTRATOR`, console-allowed (`cs_forge.cpp:178-220`). `ForgeCommandScript::GetCommands` (`:174`) defines the table; the
empty sub-command name (`:220`) is an alias of `forge help`. Handlers pass a `LineSink` (`Reply(handler)` sends each line
with `SendSysMessage`) to the `Forge::Command*` functions. A command that starts or stops a plan only records a request
(`_request`, `_requested`, `_pauseRequested`, `_resumeRequested`); `OnUpdate` applies it. Idle-only commands print a refusal
and still return `true` (the command "succeeds" from the console's and SOAP's point of view).

`ForgeMoveRecorderScript` (`cs_forge.cpp:116`) is also registered by `AddSC_forge_commandscript` (`:1252`): a
`MovementHandlerScript` that records one Playtest player's movement packets for `forge controller record`.

## Command table

"Idle only" means the handler refuses unless `Forge::IsIdle()` (no plan running or paused). Handler = `cs_forge.cpp` line
unless a file is named; body = `ForgeCommands.cpp` line.

| Command | Arguments | Idle only | What it does | Handler : body |
|---|---|---|---|---|
| `forge help` (and bare `forge`) | - | no | prints the command table text (static strings) | :231 |
| `forge status` | - | no | running: polls the learner, then `ProgressMonitor::Report` (not periodic); idle: settings table (queues, policy, envs, learner placement, socket, log, runs, models, fast summary, progress interval, last plan); then export state, pending pause/cancel/skip, cluster workers | :297 : `CommandStatus` 308 |
| `forge scenarios` | - | no | table of every stage: run state (`finished (<reason>)`, `checkpoint (resumable)`, or live state), env steps, best score, age, from `runs/<stage>/finished.json`, `latest.pt`, `progress.json`; counts exported `.amdl` models | :303 : 379 |
| `forge start` | `[scenario ...]` (space or comma separated) | no (refuses unless Idle and no pending start) | queue a plan with `AnimusForge.Policy`; no names = `AnimusForge.Queue` (else every stage), skipping stages whose `finished.json` says advanced when `Queue.SkipFinished`; refuses if any stage of the curriculum is invalid (`CurriculumProblems`); warns on seed order (`WarnSeedOrder`). Entries do not resume | :309 : 431 |
| `forge fast` | `[budget] [scenario ...]` (`20M`, `500k`, plain count >= 1000) | no | like start, but with `ForgeConfig::FastProfile(budget)`: `Fast.Envs`, learner `--overlay` `configs/fast.yaml`, `convergence.patience=0`, output to `Fast.OutputDir`; names default to `Fast.Queue` else every stage | :314 : 495 |
| `forge resume` | `[scenario ...]` | no | Paused: unpause. Training with no learner and `Learner.AutoStart`: restart only the learner from `latest.pt` (re-deals cluster ranks). Idle: queue a plan whose first entry resumes `runs/<stage>/latest.pt`, the rest start fresh; no names = first non-done entry of the last plan | :319 : 558 |
| `forge pause` | - | refuses when Idle | sets `_pauseRequested`; applied after the current decision; not sent to workers | :324 : 695 |
| `forge cancel` | - | refuses when Idle (but cancels a queued start) | `Request::Cancel`: learner saves `latest.pt` when the socket closes, plan ends, workers get `STOP` | :329 : 717 |
| `forge skip` | - | refuses when Idle | `Request::Skip`: ends the current entry (`Skipped`) and starts the next | :334 : 743 |
| `forge run` | `<scenario> <policy> [episodes]` | no | a local-policy plan (`random` only); `remote` is refused; 0 episodes = until cancel | :339 : 763 |
| `forge talents` | `<class> [spec] [points] [plan]` | no | prints a talent build (plan `standard`, `noisy`, `random`; points default 71) | :1188 : 798 |
| `forge bench` | `[scenario]` | refuses unless Idle | sim-only trials of every `Bench.Threads` x `Bench.Envs`, then the best `Bench.LearnerTop` with the learner; writes `<OutputDir>/bench/bench.json` | :1197 : 891 |
| `forge bench apply` | - | refuses while benching | writes the winner into the configs (`MapUpdate.Threads` into worldserver.conf; `AnimusForge.Envs`, `Learner.TorchThreads` into the legacy module conf or worldserver.conf) | :1197 : 975 / 989 |
| `forge bench auto` | `[scenario]` | refuses unless Idle | bench, then apply and use at once | :1197 : 891 (`apply = true`) |
| `forge export` | `[scenario] [best\|latest]` | no (refuses if an export runs) | spawns `python -m animus.export --checkpoint <run>/best.pt|latest.pt --out ModelDir --layouts-dir ...` in `Learner.WorkDir`, log `animus-export.log` beside the learner log | :1209 : 1059 |
| `forge clean archive` | - | no (refuses while a learner is starting) | deletes `<OutputDir>/archive` and legacy `runs/_archive` | :1220 : 1146 |
| `forge clean scenario` | `<scenario>` | refuses for the running or queued stage | deletes `runs/<scenario>/` | same |
| `forge clean exports` | - | refuses while exporting | deletes `*.amdl` and `*.json` in `ModelDir` | same |
| `forge clean fast` | - | refuses while a fast plan runs/queued | deletes `Fast.OutputDir` | same |
| `forge clean logs` | - | refuses while learner/export runs | deletes learner log, `.rank1..15` logs, export log | same |
| `forge clean all` | - | requires idle and no learner/export | deletes every directory under `runs/`, exports, fast, logs (not `archive`) | same |
| `forge progress` | `[seconds\|off]` | no | show or set the periodic report interval (default 0 = off) | :1225 : 1291 |
| `forge tasks` | - | no | per-map update-task totals since the previous `forge tasks` (resets them), slowest mean first | :903 |
| `forge route` | `<map> <x> <y> <z> <x> <y> <z>` | yes | creates the grids between the ends and prints `RoutePlanner::Report` | :1143 |
| `forge controller probe` | `<map> <x> <y> <z> [facing]` | yes | prints the controller's world query at a point | :708 |
| `forge controller record` | `<player> <file>` or `stop` | no | record a Playtest player's movement packets; `stop` writes the file | :763 |
| `forge controller replay` | `<file> [player]` | yes | replay a recording through the controller; drift report | :822 |
| `forge camera snapshot` | `<map> <x> <y> <z> <yaw> [pitch] [zoom] [file]` | yes | one camera frame to `<file>-depth.pgm/-kind.ppm/-height.pgm` | :467 |

Parser notes: `Tail scenarios` is split on space, comma and tab (`SplitNames`, `:71`). A `forge fast` first word that parses as a
budget is eaten as one (`ParseBudget`, `ForgeCommands.cpp:64`); numbers below 1000 are not budgets. `forge export best` is
rewritten to scenario empty, checkpoint `best` (`:1214`). `IsRunName` (`ForgeCommands.cpp:219`) rejects `.`, `..`, `_archive`
and any path separator.

Cluster: the commands act on this machine's sim only. `forge start` on a host makes the registered workers run the stage
(`DealClusterLearners`); on a worker it trains the worker alone and is overridden by the host's orders. `forge pause` is not
forwarded.

## `Forge::CommandStatus` and the report (`A/Console/Progress.cpp`)

`ProgressMonitor::Report(config, SimSnapshot, plan rows, info, warn, periodic)` (`Progress.cpp:390`):

1. Header line `Forge: <scenario> (i of n) | <state> | learner <phase> | update N | <duration>` (`ReportTraining` `:437`).
2. A `Metric | Value | Note` `TextTable`: rows `learner`, `sim`, `per decision`, `sim parts`, `reset parts`, `world parts`,
   `observe blocks` (top block by thread time), `map tasks`, `controller`,
   `vision`, `mental map`, `wire`, `camera audit`, `eval videos`, `placement` (reset percentiles), then the learner's numbers:
   `env steps`, `step rate`, `ETA (step limit)`, `ETA (converged, earliest)`, `converged`, `weakest`, `eval score`, `best vs
   baseline`, the stage headline measures with targets (`met`/`not yet`), ladder alarms, excluded classes, stand-in, `reward/
   decision`, `entropy`, `value loss`, `approx KL / clip`, `episodes/update`, and (without a headline) a fixed list of
   episode columns (`EPISODE_COLUMNS`, `:157`: dps, killed, died, time_to_kill, damage_taken, kills, pulls_cleared, wipes,
   owner_deaths, healing, owner_healing; whether the live stages still emit these is UNVERIFIED).
3. For a local policy `ReportLocal` (`:717`) shows the same sim rows and the episode means.
4. A plan table when the plan has more than one entry (`ReportPlan`, `:775`): scenario, status, env steps, best score, ETA
   (current from the live rate; pending ones from `ConfiguredTotalEnvSteps`).
5. Warnings, each one line prefixed `Warning:` to the `warn` sink: step rate drop > 30 % below the EMA, learner silent for 120 s,
   learner exited unexpectedly, reset stall, entropy under 25 % of its first value, convergence about to anneal, best below
   the baseline after 2 evaluations, approx KL > 0.05 or clip fraction > 0.30, non-finite values named in `nonfinite`.

Periodic reports (`MaybeReport`, `AnimusForge.cpp:1384`) run only when `AnimusForge.Progress.Interval` (default 0) or `forge
progress N` is set, and advance the trend state (EMA with alpha 0.3, previous reward/entropy); `forge status` never does.
`Report` also runs once at a stage's end (`ReportStageEnd`).

`progress.json` is **written only by the learner** (`apps/forge/python/animus/progress.py`, atomic write via a `.partial`
rename; NaN/Inf become null and are named in `nonfinite`). The C++ side only reads it: `ProgressFile::Load/Parse` keeps
top-level numbers, booleans (1/0) and strings, ignores nested values and nulls (`Progress.cpp:194-233`; NaN allowed from Boost
1.82). The report treats a `progress.json` whose `started_at` is older than `now - ScenarioSeconds - 5 s` as the previous
run's and ignores it (`:400`). Keys read by the status (all via `Number`/`Text`): `phase, update, env_steps, total_env_steps,
resumed_env_steps, started_at, updated_at, env_steps_per_sec, patience, eval_every, evals_since_best, evals, window,
last_eval_env_steps, converged_layouts, weakest_layout, weakest_missing, last_eval_score, best_score, best_env_steps,
baseline_score, baseline, status_headline, status_targets, status_excluded, stand_in, scenario, eval_<metric>,
episode_<metric>, ladder_collapsed, ladder_stalled, reward_per_decision, entropy, value_loss, approx_kl, clip_frac, episodes,
nonfinite, episode_<EPISODE_COLUMNS>`. `forge scenarios` and the plan table also read `finished.json` (`reason`, `advanced`).
`RunAdvanced` treats a missing or non-zero `advanced` as advanced (`AnimusForge.cpp:985`).

`ConfiguredTotalEnvSteps` (`Progress.cpp:293`) finds a stage's step limit by regex over the YAML (`^total_env_steps:` and
`^extends:`, following `extends` up to 16 links), then an `--overlay` file, then a `--set total_env_steps=` in the learner args.

## `TextTable` and `Format` (`A/Console/TextTable.*`)

`TextTable(columns)`; `AddRow` pads/truncates to the column count; `Lines(indent)` widths by byte length (ASCII only), columns
separated by two spaces, header underlined with dashes, trailing spaces trimmed; `Write(sink, indent)`. `Format::Count`
(thousands commas), `Compact` (`k` from 1e4, `M`, `B`), `Fixed`, `Metric` (adaptive digits), `Duration` (`45s`, `12m 04s`,
`3h 55m`, `2d 07h`), `Percent`, `Cpus` (mask of CPUs 0-63 as ranges), `OrDash`. No tests.

## Observed issues

- `cs_forge.cpp:238-239` help for `forge fast` says "default: AnimusForge.Queue"; the code uses `AnimusForge.Fast.Queue`
  (`ForgeCommands.cpp:522`). `:284` help for `forge clean archive` says "runs/_archive/"; the code deletes
  `<OutputDir>/archive` and the legacy directory (`ForgeCommands.cpp:1164`).
- `ForgeCommands.cpp:1220-1266`: `forge clean scenario` and `forge clean all` delete run directories (checkpoints included)
  with no confirmation.
- `ForgeCommands.cpp:798-889`: `forge talents` uses `ClassAssets::For`, which `WarmCaches` already builds for every class at startup (`Scenario/Curriculum/WarmCaches.cpp:40`), so it does not query the sealed databases.
- `Progress.cpp:717-773` repeats `ReportTraining`'s sim rows verbatim (duplicated code).
- `Progress.cpp:153-154,576-585`: baseline comparison rows persist although only `random` can be a baseline.
- `apps/forge/python/animus/progress.py:7` still cites `src/Console/Progress.cpp`.
- `Progress.cpp:268-269`: the YAML scan only sees top-level, column-0 `total_env_steps:`/`extends:`; a list-valued `extends` or
  indented form is missed (the learner's own parser may accept more).

## Reviewer notes

- Idle-only refusals return success; if scripts (SOAP) depend on the exit status, they cannot tell. Question: return false?
- `status` reads `progress.json` from disk on every call, including once per plan row for finished entries.
- Tests: none for `Progress`/`TextTable`; Python `test_progress.py` pins the writer's format.
