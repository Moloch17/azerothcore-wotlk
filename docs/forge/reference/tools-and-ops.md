# Tools, forgectl, build and ops layout

Purpose and scope. Everything around the code that is not the sim or the learner: the scripts in `apps/forge/tools`, the
`forgectl` program (a code map; usage is in [../forgectl.md](../forgectl.md)), the cluster file, the container and
compose files, the patches, the model data, the repository's top-level layout, the git conventions and the
build. Related: [config-keys.md](config-keys.md), [tests.md](tests.md) (there are no tests),
[../cluster.md](../cluster.md),
[../deploy-gate.md](../deploy-gate.md), [00-architecture.md](00-architecture.md), [known-issues.md](known-issues.md).

## Map of the files in this area

| Path | Lines | Role |
|---|---|---|
| `forge.sh` | 109 | start/stop/attach the training container; `--build` recompiles inside it |
| `forgectl` | 8 | shim that puts `apps/forge` on the path and runs the `forgectl` package |
| `apps/forge/cluster.toml` | 96 | the machines, host, remote, branch, ports, container and paths forgectl reads, plus the `[doctor]` thresholds and the `[notify]` sinks of `watch` |
| `apps/forge/forgectl/__main__.py` | 205 | argument parser, dispatch, audit wiring |
| `apps/forge/forgectl/__init__.py` | 1 | package marker |
| `apps/forge/forgectl/config.py` | 182 | loads and validates `cluster.toml` into `Config` / `Machine` |
| `apps/forge/forgectl/remote.py` | 84 | ssh (BatchMode) and local command execution, `parallel_map` |
| `apps/forge/forgectl/ui.py` | 58 | printing, the `Proceed? [y/N]` confirmation, tables |
| `apps/forge/forgectl/audit.py` | 142 | `~/.forgectl/audit.log` intent/result lines |
| `apps/forge/forgectl/home.py` | 10 | `forgectl_home()`: `$FORGECTL_HOME` or `~/.forgectl` |
| `apps/forge/forgectl/console.py` | 275 | types one line into a worldserver console through `docker attach` under a pty; per-machine flock; signal guard |
| `apps/forge/forgectl/cluster.py` | 200 | `forgectl cluster`: one read-only probe per machine, table |
| `apps/forge/forgectl/watch.py` | 303 | `forgectl watch`: `diff` of two status documents into events, the state file, the poll loop |
| `apps/forge/forgectl/notify.py` | 121 | the `[notify]` sinks: desktop, command, webhook, file |
| `apps/forge/forgectl/doctor.py` | 272 | `forgectl doctor`: the read-only pre-flight checks |
| `apps/forge/forgectl/confkeys.py` | 79 | the conf keys that decide the sim's cost and sight (must match / may differ), shared by cluster, conf-sync and doctor |
| `apps/forge/forgectl/snapshot.py` | 371 | `status --json` / `cluster --json`: the schema-1 document built from `progress.json`, `finished.json`, `spec.json`, `metrics.csv`, `eval.csv` and the machine probes |
| `apps/forge/forgectl/stage.py` | 238 | `forgectl stage ...` and `forgectl status` |
| `apps/forge/forgectl/deploy.py` | 359 | `forgectl build [--cluster]` and `cluster move-host` |
| `apps/forge/forgectl/confsync.py` | 262 | `forgectl conf-sync`; the conf writers (also used by move-host) |
| `apps/forge/forgectl/logs.py` | 96 | `forgectl logs` |
| `apps/forge/forgectl/videos.py` | 48 | `forgectl videos` (wraps `collect-videos.sh`) |
| `apps/forge/tools/cluster-pull.sh` | 51 | on one machine: pull code, recreate the worldserver container |
| `apps/forge/tools/collect-videos.sh` | 84 | pull evaluation videos from workers by ssh+tar |
| `apps/forge/tools/conf_prune.py` | 311 | unknown-key finder and conf cleaner (local or over ssh) |
| `apps/forge/tools/resume_check.py` | 498 | CPU dry run of `--resume`, or of a fresh start, for a stage |
| `apps/forge/tools/run_snapshot.py` | 179 | a run's headline readings; before/after comparison |
| `apps/forge/tools/sim_metrics.py` | 586 | extracts the metric names a stage's sim produces from the C++ |
| `apps/forge/tools/stage_json_diff.py` | 256 | diff of two `stage.json` files |
| `apps/forge/tools/gen_config_reference.py` | 422 | regenerates the key table of [config-keys.md](config-keys.md) |
| `apps/forge/tools/runtime_graph_check.py` | 59 | exits 1 listing every `Animus/Runtime/` file that includes a header outside `Runtime/` (the runtime/training cut line) |
| `apps/forge/patches/mod-animus-amdl8.patch` | 840 | patch for the `mod-animus` module (model format version 8/9 reader) |
| `apps/forge/patches/amdl8-check/{README.md,prep.py,run.py,bench.py,golden.cpp}` | 1, 44, 38, 34, 157 | harness that checks the module's model reader against the learner's golden vectors |
| `docker-compose.yml` | 284 | services `ac-database`, `ac-worldserver`, `ac-dev-server`, stock profile services |
| `docker-compose.cluster.yml` | 25 | host-network override for cluster machines |
| `docker-compose.override.yml` | (untracked) | per-machine GPU devices, torch index, `CCUSTOMOPTIONS` |
| `apps/docker/forge-worldserver.sh` | 95 | the worldserver container's command: build if needed, venv, TensorBoard, exec worldserver |
| `apps/docker/animus-venv.sh` | 42 | creates the learner venv and installs torch and the learner (and pytest: the script still installs the `dev` extra, `animus-venv.sh:41`) |
| `apps/docker/Dockerfile.dev-server` | 120 | the `dev` image shared by `ac-worldserver` and `ac-dev-server` |
| `apps/docker/Dockerfile`, `docker-cmd.sh`, `entrypoint.sh`, `README.md` | 279, 216, 54, 41 | upstream AzerothCore images (stock profile) |
| `src/server/game/CMakeLists.txt` (lines 53-73) | n/a | `FORGE_PYTHON_DIR` define and the `ForgeSourceHash.h` step |
| `src/cmake/compiler/{clang,gcc}/settings.cmake` | n/a | `-march=native` and `-O3` forge flags |

Data directories that are not tracked: `apps/forge/models/` (270 files, 580 MB, exported `.amdl` models and `.json` manifests of the
first curriculum, e.g. `deathknight_companion`, `deathknight_duel`). It is listed in `.gitignore`.

## Top-level repository layout

Forge-specific vs upstream is from `git diff --name-status master HEAD` (master is upstream AzerothCore).

| Path | Origin | What it is |
|---|---|---|
| `src/server/game/Animus/` | forge (added) | the sim: bots, envs, bridge, movement, vision, scenario ([00-architecture.md](00-architecture.md)) |
| `src/server/game/Forge/` | forge (added) | `ForgeCore`: playtest flag, seal, tick control ([01-forge-core-delta.md](01-forge-core-delta.md)) |
| `src/server/apps/worldserver/` | upstream, with `ForgeMain.cpp` and a forge `worldserver.conf.dist` | entry point; `ForgeMain.cpp` replaces upstream `Main.cpp` in the build |
| `src/server/{game,scripts,database,shared}`, `src/common` | upstream, 173 modified and 232 added files under `src/server`, 17 modified under `src/common` | AzerothCore with forge hooks |
| `apps/forge/` | forge | learner (`python/`), `forgectl`, tools, patches, `cluster.toml` (207 added files) |
| `apps/docker/` | upstream, 2 added (`forge-worldserver.sh`, `animus-venv.sh`), 2 modified | container files |
| `apps/{compiler,installer,codestyle,...}` | upstream | AzerothCore helper scripts (`acore.sh` drives `apps/compiler`) |
| `modules/` | upstream loader files tracked; content ignored | holds `mod-animus` (the shipping module for a stock realm) and `mod-animus-forge` (a dead checkout: the forge became core code, see [01-forge-core-delta.md](01-forge-core-delta.md)) |
| `conf/dist/` | upstream, 2 modified | distributed config templates (`env.ac` etc.); `conf/*.conf` is ignored |
| `data/sql/` | upstream; 65 files deleted on this branch | SQL base and updates (immutable per AGENTS.md) |
| `deps/` | upstream, boost modified | vendored libraries |
| `docs/forge/` | forge | these documents |
| `doc/` | upstream | changelog, ConfigPolicy.md, Logging.md |
| `bin/`, `acore.sh`, `acore.json`, `install.sh`, `flake.nix`, `PreLoad.cmake` | upstream | AzerothCore dashboard, installer, nix |
| `tools/socket_stress_heavy.py` | upstream | stress script |
| `.agents/`, `AGENTS.md`, `CLAUDE.md`, `.claude/skills` | upstream agent docs, modified | agent rules; `.agents/plans/**` is ignored (plans are local) |
| `.github/`, `e2e/` | upstream; `e2e/` (39 files) deleted on this branch | CI |
| `forge.sh`, `forgectl`, `docker-compose.yml`, `docker-compose.cluster.yml` | forge (added, compose yml modified from upstream) | entry points |
| `env/` | local, ignored | install tree (see below) |
| `var/` | local, ignored | scratch and data (see below) |

## Gitignored areas

From `.gitignore` (excerpt): `/conf/*` except `conf/dist`, `/modules/*` except loader files, `/build*/`, `/var/*`
(except
`.gitkeep` files in `var/build` and `var/ccache`), `/env/dist/*`, `/env/user/*`, `/.env*`, `/data/sql/custom/*`,
`/*.override.yml`, `*.patch` and `*.diff`, `.agents/plans/**` (with a few exceptions), `.claude/worktrees`,
`apps/forge/models/`, `apps/forge/python/{runs,layouts,.venv}/`.

- **`env/dist/`** (the install tree inside the container at `/azerothcore/env/dist`): `bin/worldserver`,
  `bin/libforge-gpu.so` (the optional device library), `bin/Data` (client data mount), `etc/` (conf files, see
  [config-keys.md](config-keys.md)), `logs/` (`Server.log`, `Errors.log`, `animus-learner.log`, `tensorboard.log`),
  `data/` (maps, vmaps, mmaps: a docker volume mounted read-only), plus upstream scripts (`starter`, `run-engine`, ...).
  Marker files: `env/dist/.forge-build` (a build request) and `.forge-build-cpu` (CPU signature the binary was
  built for; `forge-worldserver.sh:38-57`).
- **`env/user/`**: empty on the dev machine.
- **`var/`**: `var/animus-forge/` is the default `OutputDir` in compose
  (`shared/{runs,archive,layouts,bench,fast}`;
  `shared/runs/<stage>/` holds `progress.json`, `metrics.csv`, `eval.jsonl`, `latest.pt`, `best.pt`, `stage.json`),
  `var/build` and `var/ccache` are the compose volumes' mount points for the build tree and ccache (`docker-compose.yml`
  volumes), `var/client` is the extractor client folder, `var/syntax-*`/`var/forgectl-build-*` are throwaway cmake
  trees (the owner's syntax-check helper: UNVERIFIED how `var/syntax-forge` is made), the rest
  (`m3`, `lakes`, `smoke`, `prof`, `realm-*`, `stock-*`, `model-archive`, `backups`, `bench-vision`, `camera`, `cores`,
  `cfgcheck`, `claude`, `cluster-worker-logs*`, `extractors`, ...) are ad hoc scratch: UNVERIFIED purpose of each,
  none is read by repository code except as listed here. `var/gate/NOTES.txt` is where deploy-gate.md asks the
  operator to write notes (not present on the dev machine now).
- **`modules/`**: `mod-animus` (the shipping module for the live realm: `animus-lib`, `src`, `models`, `tools`,
  `tests`),
  `mod-animus-forge` (dead; the forge became core code), plus the tracked loader files.
- **`.agents/`**: tracked `docs/`, `skills/`, `README.md`; `plans/` ignored (planning docs per AGENTS.md).

## Git and branch conventions

- `forge` is the working and release branch; `master` is upstream AzerothCore and is never pushed to or used
  ([principles.md](../principles.md) item 20). Merges go on `forge`; no pull requests unless asked.
- Remotes on the dev machine: `lan` = `/home/moloch/git/animus-forge.git` (bare repo the workers pull from; cluster.md
  writes it as `moloch@192.168.0.69:git/animus-forge.git`), `origin` = GitHub fork `Moloch17/azerothcore-wotlk`,
  `upstream` = `azerothcore/azerothcore-wotlk`.
- Tags that matter: `curriculum-v1`, `pre-cleanup-2026-10-07`, `curriculum-movement-v1`, `archive/movement-curriculum`.
- Agent worktrees live under `.claude/worktrees/` on branches `worktree-agent-*`; `.claude/worktrees` is ignored.
- Patches: `.gitignore` ignores `*.patch`, yet the `apps/forge/patches/*.patch` file is tracked
  (`git ls-files apps/forge/patches` lists them): they were force-added; a new patch there would not be picked up by
  `git add -A`.

## The build

1. `./forge.sh --build` (`forge.sh:86-92`): creates `env/dist`, touches `env/dist/.forge-build`, runs
   `docker compose up -d --force-recreate ac-worldserver`, then polls (`wait_for_build`, `forge.sh:48-69`) until the
   container
   removes the request file (success: prints the binary's mtime) or stops (failure: prints the compiler errors and exits
   1). It attaches to the console only from a terminal.
2. In the container, `apps/docker/forge-worldserver.sh` decides to build when there is no `env/dist/bin/worldserver`, or
   the request file exists, or the CPU signature changed (`forge-worldserver.sh:47-55`). `build_worldserver` runs
   `acore.sh compiler configure` then `acore.sh compiler compile` (lines 30-36); `CTYPE` is `RelWithDebInfo` unless
   `FORGE_CTYPE` is set (line 24; `conf/dist/env.ac` sets `CTYPE=Release` for upstream containers, `CSCRIPTS=static`,
   `AC_CCACHE=true`). The request file is removed only after a successful build (line 59), so a failed build is retried
   by
   the next `--build`.
3. **Source-hash step.** `src/server/game/CMakeLists.txt:62-73` globs `src/server/game/Animus/**/*.cpp,*.h,*.hip` at
   configure time, SHA-256s each, hashes the concatenation, keeps 16 hex characters and writes
   `ForgeSourceHash.h` (`#define FORGE_SOURCE_HASH`), included only by `AnimusForge.cpp`. This is the `src=` part of the
   cluster fingerprint ([config-keys.md](config-keys.md)). It does not cover `ForgeMain.cpp`, core files outside
   `Animus/`, the Python learner, the yaml configs or `ForgeConfig` data: a change only there leaves the fingerprint
   (and so the "same code" check) unchanged. Because the hash is computed at configure time, every build configures
   first
   (`forge-worldserver.sh:26-36`).
4. **`-march=native`.** `src/cmake/compiler/{clang,gcc}/settings.cmake` set `-march=native` plus `-O3` for non-Debug on
   x86-64, and `-mtune=znver5` (or `znver4`) when the CPU is AMD family 26. So every machine must build for itself;
   a binary copied between different CPUs may die on an unsupported instruction. `forge-worldserver.sh:38-46, 53, 57`
   stores an
   md5 of `/proc/cpuinfo` vendor/family/model/flags in `env/dist/.forge-build-cpu` and rebuilds when it differs.
5. `FORGE_PYTHON_DIR` (`src/server/game/CMakeLists.txt:55-56`) bakes `${CMAKE_SOURCE_DIR}/apps/forge/python` into the
   binary as the default learner work directory (and via its parent, models).
6. The compose `ac-worldserver` mounts the whole checkout at `/azerothcore` (`docker-compose.yml`
   `${DOCKER_VOL_ROOT:-.}:/azerothcore:cached`), the volumes `ac-animus-forge-build-dev` at `/azerothcore/var/build`
   and `ac-animus-forge-ccache-dev` at `/azerothcore/var/ccache`, and the client-data volume at `env/dist/data`
   read-only. The override sets `CCUSTOMOPTIONS=-DMODULE_MOD-ANIMUS=disabled` so `mod-animus` is never built into the
   forge core.

## Containers

| Container | From | Runs |
|---|---|---|
| `ac-animus-forge-database` | `mysql:8.4`, compose service `ac-database` | MySQL; published on `127.0.0.1:13306`; healthcheck `SHOW DATABASES` |
| `ac-animus-forge-worldserver` | compose `ac-worldserver` (image `ac-animus-forge-dev-server`) | `apps/docker/forge-worldserver.sh`: the sim; the learner is a child process of the worldserver; TensorBoard on loopback 16006; `stdin_open`+`tty` for the console; `init: true`; `restart: "no"`; `stop_grace_period: 1m`; `shm_size: 8gb` |
| `ac-animus-forge-dev-server` | compose `ac-dev-server`, profile `dev` | the toolchain with the tree mounted; runs `animus-venv.sh` and a shell |
| `ac-animus-forge-client-data-init` | compose `ac-client-data-init` | populates the `ac-animus-forge-client-data` volume |
| `ac-animus-forge-db-import`, `-authserver` | profile `stock` | upstream services, not used for training |
| `ac-animus-forge-tools` | profile `tools` | map extractors |
| `claude-syntax` | not defined in this repository | the container the deploy-gate commands `docker exec` into (formerly also `forgectl test`) (`cluster.toml` `[dev]`); UNVERIFIED how it is created |

`docker-compose.cluster.yml` (use on every cluster machine, in `.env`:
`COMPOSE_FILE=docker-compose.yml:docker-compose.override.yml:docker-compose.cluster.yml`) switches `ac-worldserver` to
`network_mode: host`, resets its networks and ports, points the database at `127.0.0.1:13306` and sets
`FORGE_LOCAL_ONLY=1` (TensorBoard stays on loopback). Ports 7700-7702 must be open between machines.

The dev machine's `docker-compose.override.yml` (untracked) passes only the discrete GPU by PCI path as
`/dev/dri/renderD128` and `card0` plus `/dev/kfd`, adds the render and video groups, sets `ROCR_VISIBLE_DEVICES=0`, the
ROCm torch index and `CCUSTOMOPTIONS` for both `ac-worldserver` and `ac-dev-server`.

## Tools

Safety column: "reads" = never writes outside stdout (or a named output); "writes" names what.

| Tool | Purpose | Arguments | Inputs / outputs | Safety |
|---|---|---|---|---|
| `conf_prune.py` | list/comment-out `AnimusForge.*` keys the build no longer reads; list/restore backups; which keys a rev range removed | `--removed OLD NEW`; `--check CONF`; `--prune CONF`; `--ssh user@host:PATH`; `--list-backups`; `--restore STAMP`; `--dist FILE`, `--dist-rev REV`, `--old REV`, `--no-stage-check`, `--repo` (`conf_prune.py:282-295`) | reads `worldserver.conf.dist` (checkout or `git show REV:path`), the C++ for key families, the conf; `--prune` writes `<conf>.bak-<stamp>` then the conf with unknown lines turned into `#pruned <stamp> (<why>): <line>` (never deleted); `--restore` keeps the current file as `<conf>.pre-restore-<now>` | `--check`, `--removed`, `--list-backups` read; `--prune`/`--restore` write the conf (local or over ssh, BatchMode) |
| `resume_check.py` | dry-run `forge resume` or a fresh start of a stage on CPU, no sim | `RUN_DIR`, `--stage-json`, `--config`, `--spec`, `--checkpoint`, `--set`, `--overlay`, `--fresh`, `--stage`, `--all`, `--stage-json-dir`, `--state-dim`, `--goal-count` (`resume_check.py:458-470`) | reads checkpoint, stage.json, yaml; prints PASS/FAIL per check; exit 0/1/2; `STATE_DIM` and `GOAL_COUNT` constants stand in for values stage.json lacks (UNVERIFIED: whether the constants match the live sim) | reads only (docstring: never writes into RUN_DIR); needs torch |
| `run_snapshot.py` | a run's readings (last evaluation headline, medians of `approx_kl`, `env_steps_per_sec`, `update_seconds`, `entropy`, rung) and a before/after comparison | `RUN_DIR [--config] [--last N] [--json OUT]`; `--compare BEFORE AFTER` (`run_snapshot.py:157-161`) | reads `metrics.csv`, `eval.jsonl`, yaml; `--json` writes OUT | reads, plus OUT |
| `sim_metrics.py` | the metric names each stage's sim reports, parsed from the C++ (`Stages.cpp`, `StageScenario.cpp`, `StandInSeat.cpp`, `Encounters/*.cpp`, `CombatReward.cpp`); `--check` compares with real `stage.json` `episode_info` | `--stage NAME`; `--check STAGE_JSON...` (`sim_metrics.py:547-548`) | stdout | reads |
| `stage_json_diff.py` | what changed between two `stage.json`: header, layouts, shapes, actions, blocks, obs names, sets, episode info, categories, reward terms, tuning, arenas | `old.json new.json [--allow-removed-terms] [--allow-removed-keys] [--allow-removed-columns]` | stdout; exit 0 identical/allowed, 1 other, 2 bad input | reads |
| `gen_config_reference.py` | regenerate the key table in [config-keys.md](config-keys.md) | `--check`, `--stdout` | reads conf.dist and sources; writes only between its two marker lines | writes one docs file |
| `collect-videos.sh` | copy workers' `runs/<stage>/videos` PNGs, JSON and HTML by `ssh ... find ... \| tar` into `runs/<stage>/videos/from-<worker>/` | `[--dry-run \| --check] [--workers "u@h ..."] [--remote-dir D] [--runs-dir D] <stage>` | default workers list includes sarah (the host) and omits eli (`collect-videos.sh:23`) | `--dry-run` prints; `--check` read-only ssh; default writes the local run folder |
| `cluster-pull.sh` | on one machine: `git pull --ff-only <origin user@host>:git/animus-forge.git forge`; touch `env/dist/.forge-build`; `docker compose up -d --force-recreate ac-worldserver` | `[user@host]` | modifies the checkout and containers of the machine it runs on | writes |
| `patches/amdl8-check/*` | compare the in-game model reader (`mod-animus` `MlpPolicy`) to the learner's golden vectors; time a decision | `prep.py <dir>`, `run.py <dir> [Model dir]`, `bench.py <dir>` | needs `modules/mod-animus` and `var/syntax-build-animus/compile_commands.json` | writes in `<dir>` |

## forgectl code map

Usage and behaviour are in [../forgectl.md](../forgectl.md); this is where each piece lives.

- `__main__.py`: `parser()` defines `cluster [--all] [--json] [move-host]`, `status [--json]`, `doctor`, `watch [--once]`, `stage {status,start,resume,pause,cancel}`,
  `logs`, `build`, `conf-sync`, `videos` (`forgectl test` and `testcmd.py` were removed with the test suites, see
  [tests.md](tests.md)). `changes_state` (`__main__.py:125-138`) decides which invocations are
  audited
  (everything except read-only forms); `planned_machines` names the machines on the intent line; `main` writes the
  intent before running and the result in `finally`, and maps `ConfigError/Failure/AuditError` to exit 1, Ctrl-C to 130.
- `config.py`: reads `cluster.toml` with `tomllib`; `Config.cluster` = machines with `in_cluster`, `workers` = cluster
  minus the host, `path_of(machine, key)` joins the machine's checkout and a `[paths]` entry; `ROLES = host|worker|dev`.
- `remote.py`: `execute(argv, input, timeout)` runs a local process; `on(machine, script)` runs a script locally if
  `machine.local` else `ssh -o BatchMode=yes` (`ssh_argv` adds the connect timeout, `-tt` for a tty);
  `parallel_map` is a thread pool.
- `console.py`: `send(config, machine, line)` locks `~/.forgectl/locks/<machine>.lock` (`machine_lock`, flock, 90 s),
  spawns `docker attach --sig-proxy=false` under `pty` (via ssh for a worker), types the line, reads to the `AC> `
  prompt, strips colour and log noise (`parse_reply`), and detaches with Ctrl-P Ctrl-Q in a `finally`
  (`detach`); `SignalGuard` turns SIGTERM/SIGHUP into an exception so the detach runs.
- `cluster.py`: one shell probe per machine in parallel (revision, container state, last learner log line, load,
  disk, GPU memory, docker restart policy, the cadence keys of the conf), `refused_lines` greps the host's logs for "refused the worker".
- `snapshot.py`: `collect` builds the schema-1 document of `status --json` (one ssh read of the newest run's
  `progress.json`, `finished.json`, `spec.json`, last `metrics.csv` row and `eval.csv`; the machine probes; the
  refusal lines; optionally one console `forge status`); `cluster_json`, `status_json` print it. Optional learner keys
  (`wall_steps_per_sec` ...) are read with `num()` and are `null` when absent.
- `confkeys.py`: `scan` the must-match and may-differ conf keys of a conf text, `cadence`, `mismatches`; the probe's
  shell `GREP` sends only those lines.
- `watch.py`: `conditions` (what is wrong now, keyed by kind and subject), `transitions` (what changed between two
  polls), `diff` (both, with the persisted `active`/`streak` state so a condition is told once, after
  `confirm_polls` for worker drops), `tell` (print, min_severity, debounce, sinks), `run` (lock, poll loop, state file).
  `notify.py`: the four sinks and `Event`.
- `doctor.py`: `checks()` turns the gathered facts into PASS/WARN/FAIL lines (thresholds in `config.DOCTOR_DEFAULTS`,
  `cluster.toml` `[doctor]`); `run` gathers in parallel and exits 1 on a FAIL. Read-only.
- `stage.py`: `console_line` builds `forge start|resume|pause|cancel`; `start` shows what it archives
  (`existing_run`, `ARCHIVE_TOKEN_STEPS = 1,000,000`); `pause` and `cancel` also go to each worker's console;
  `status` prints the console table and the learner's last `update` line.
- `deploy.py`: `build` (both forms first read the plan state of the machine they restart, `stage.machine_plan_state`:
  the host for `--cluster`, this machine for a local build; refuse if a stage runs unless `--stop-running`; local:
  touch request + recreate container; `--cluster`:
  `git push <lan> <branch>`, run `cluster-pull.sh` on each machine in parallel, wait for the log line
  `AzerothCore rev. <sha9> ... ready`); `move_host` (cancel, copy run, rewrite roles, rebuild, edit `host =` in
  `cluster.toml`, resume) with `mixed_state_report`. It deploys the local `HEAD`: uncommitted work is not shipped.
- `confsync.py`: `curriculum_keys`, `compare`, `synced_text` (replace differing, append missing under a comment, remove
  extra), `set_cluster_role` (used by move-host), `write_script` (temp file + checksum + `mv`, in-place fallback for a
  bind-mounted file), `restore_command`.
- `logs.py`: one remote script collects `docker logs`, `Errors.log`, the learner log; `problems` filters with the
  `PROBLEM`
  and `NOISE` regexes.
- `videos.py`: argv builder for `collect-videos.sh`.
- `audit.py`: `begin/intent/touch/note/confirmed/finish`; the intent line must be written or the command is refused.

Config keys forgectl itself reads: none of the `AnimusForge.*` keys except `Cluster.Role` and `Cluster.Host`, which
`confsync.set_cluster_role` rewrites (`confsync.py:83-99`). Environment: `FORGECTL_CONFIG`, `FORGECTL_HOME`.

## Observed issues

- `collect-videos.sh:23` lists sarah in its default workers although sarah is the host; with `forgectl videos` run on
  the host this copies the host's own videos onto themselves (`from-sarah/`). UNVERIFIED whether that is wanted.
- `cluster.toml` repeats ports (7700-7702) that also live in each machine's conf; they are not read from the conf, so a
  changed port needs two edits.
- `cluster.toml` `[dev] container = "claude-syntax"` names a container that no tracked file creates.
- The cluster source hash omits `ForgeMain.cpp`, `src/server/game/Forge`, the learner and yaml configs (see Build 3).
- `docker-compose.yml:125` says "Every start retrains the queue from scratch" next to `restart: "no"`; stale, since the
  sim starts idle and trains nothing until `forge start` (conf.dist `AnimusForge.Queue` text).
- `.gitignore` ignores `*.patch`; tracked patches survive only because they were force-added.
- `deploy.py` `deploy_one` assumes each machine's `origin` remote is the dev machine (`cluster-pull.sh:24`); a worker
  whose `origin` points to GitHub would pull from there (UNVERIFIED: the workers' remotes were not read).

## Reviewer notes

- `forgectl` duplicates facts that live in conf files (roles, ports) and in docs (machine table). A single source
  (cluster.toml) feeding `conf-sync` for Cluster.* keys would remove the duplication.
- `console.py` is the only control path to a running sim (docker attach under a pty). decisions/0001-control-socket.md
  proposes replacing it.
- Everything that changes a machine goes through `audit.begin/intent`; any new command must be added to
  `changes_state` and `planned_machines` or it is not audited.
- Moving the source-hash to cover `apps/forge/python` and `ForgeMain.cpp` would change the fingerprint of every
  existing deployment at once (a cluster-wide rebuild is needed).
