# 7. Operations

Step-by-step procedures, rewritten 2026-10-07 from the code. For cluster work use [forgectl.md](forgectl.md),
[cluster.md](cluster.md) and [deploy-gate.md](deploy-gate.md); this chapter does not repeat them. Every command named
here exists in `cs_forge.cpp:196-220` (console) or `forge.sh`. Where something could not be verified from the repository
it is marked `UNVERIFIED`.

## 7.1 Setting up a training host (Docker)

Prerequisites (from the compose files): Linux, Docker with Compose, a checkout of branch `forge`, enough disk for the
client-data volume, builds and runs. The learner needs a GPU for speed; the compose file passes `/dev/kfd` and `/dev/dri`
(AMD ROCm, gfx1100 here) through the shared dev-image anchor; an NVIDIA card needs the commented `deploy` block instead
(`docker-compose.yml`). Machine-specific settings (GPU lines, reuse of volumes) go in a gitignored
`docker-compose.override.yml`. Set `ANIMUS_TORCH_INDEX_URL` (default ROCm 6.4 index) before the first start: the venv is
created once (`apps/docker/animus-venv.sh`).

```bash
git clone -b forge git@github.com:Moloch17/azerothcore-wotlk.git animus-forge     # path used by the cluster: ~/animus-forge
cd animus-forge
./forge.sh            # docker compose up -d, then attach to the worldserver console
```

There is no module to clone: the sim is in the core tree (`src/server/game/Animus`). The old step "clone
`animus-forge` into `modules/`" is obsolete, and `modules/CMakeLists.txt` ignores a stale `modules/mod-animus-forge`
checkout.

What the first start does (`apps/docker/forge-worldserver.sh`): builds the worldserver with `acore.sh compiler
configure` and `compile` if `env/dist/bin/worldserver` is missing, a build was requested (`env/dist/.forge-build`), or the
CPU differs from the one the binary was built for (`env/dist/.forge-build-cpu`; the build uses `-march=native`); restores
missing `.conf` files from their `.dist`; runs `animus-venv.sh`; starts TensorBoard on
`http://localhost:16006` (`FORGE_TENSORBOARD_PORT`, loopback in the bridge network) reading `<OutputDir>/runs`; then
`exec ./worldserver`. The database is created by the worldserver's own `Updates.AutoSetup`; the `stock` profile services
are not needed.

`forge.sh`:

| Command | Effect |
|---|---|
| `./forge.sh` | `docker compose up -d`, then attach |
| `./forge.sh --build` | request a build, recreate `ac-worldserver`, wait until the new binary is installed or the build failed (exit 0/1), attach only from a terminal |
| `./forge.sh attach` | attach to the console; detach with Ctrl+P Ctrl+Q, **never Ctrl+C** (it stops the server) |
| `./forge.sh dev` | also start `ac-dev-server` |
| `./forge.sh stop` | stop `ac-worldserver` (the learner saves) |

Settings come only from config: `worldserver.conf` (the `Forge.*` and `AnimusForge.*` keys are in
`worldserver.conf.dist`) and, if present, `env/dist/etc/modules/mod_animus_forge.conf`, read after it. There are no
worldserver flags. Key reference: [reference/config-keys.md](reference/config-keys.md). The cluster-machine overlay is
`docker-compose.cluster.yml` (host networking; add it to `COMPOSE_FILE` in `.env`).

## 7.2 The console

Attach (`./forge.sh attach` or `docker attach ac-animus-forge-worldserver`) and type `forge help`. Commands, from
`cs_forge.cpp:233-290`:

| Command | What it does |
|---|---|
| `forge status` | state, progress, ETA, timing breakdown, warnings (or the idle settings) |
| `forge scenarios` | every stage with its run: checkpoint, steps, best score |
| `forge start [stage ...]` | train these from scratch in order; no names: `AnimusForge.Queue`, else all twelve stages minus those already finished (`AnimusForge.Queue.SkipFinished`) |
| `forge fast [stage ...]` | fixed-budget low-cost rehearsal into `<OutputDir>/fast/` (7.5) |
| `forge resume [stage ...]` | unpause; or restart a dead learner; or continue the first named stage from its `latest.pt` and train the rest from scratch; no names: continue the last plan |
| `forge pause` | freeze the sim and the learner after the current decision. **Does not reach cluster workers** |
| `forge cancel` | stop the plan; the learner saves `latest.pt` first (a cancelled run resumes where it left off) |
| `forge skip` | end the current stage and start the next |
| `forge run <stage> random [episodes]` | the `random` policy, no learner (the only local policy) |
| `forge export [stage] [best\|latest]` | write `.amdl` models and manifests to `AnimusForge.ModelDir` |
| `forge bench [stage]`, `bench apply`, `bench auto` | time thread and env counts with and without the learner; apply the winner to the conf |
| `forge talents <class> [spec] [points] [plan]` | print a build the curriculum would give (no training) |
| `forge clean archive\|scenario <stage>\|exports\|fast\|logs\|all` | delete run data (idle only for `all`) |
| `forge progress [seconds\|off]` | the periodic report interval |
| `forge tasks` | per-map update task times since the last call |
| `forge route`, `floorscan`, `fieldroute`, `fieldstage`, `fieldworld` | route and layered-field tools |
| `forge controller record\|replay\|probe`, `forge camera snapshot\|diff`, `forge gpu scene` | diagnostics (idle only; `record` needs a playtest player) |

Rules the code enforces: commands only record a request and are applied at the start of a tick, never inside a decision;
`start` refuses while a plan is running; `start`/`resume` refuse a stage with no valid definition. The help line for
`forge clean archive` says `runs/_archive/`; the code deletes `<OutputDir>/archive` (and the legacy `runs/_archive`). The
help line for `forge fast` says the default is `AnimusForge.Queue`; the code uses `AnimusForge.Fast.Queue`, else every
stage (`AnimusForge.cpp:965-975`).

## 7.3 Running a stage

```
forge start move1_controls         fresh run; an earlier run of that stage is moved to <OutputDir>/archive/
forge status
forge cancel                       saves latest.pt
forge resume move1_controls        continue from latest.pt
```

On a cluster use `forgectl stage start|resume|pause|cancel` (it also types into the workers' consoles).
After a rebuild always `resume`, never `start` (principle 16): a start archives the run. Every run of one stage seeds
from its parent's checkpoint by name; `forge start` warns when a stage is listed before its parent or its parent has no
checkpoint (`WarnSeedOrder`). The learner config for a stage is `apps/forge/python/configs/<stage>.yaml`
(`AnimusForge.Learner.Config` overrides).

A stage ends by itself: the learner exits 0 when every class has converged or at `total_env_steps`, writes
`finished.json`, and the plan starts the next entry. See [reference/00-architecture.md](reference/00-architecture.md)
section 5.

## 7.4 Where things are

`<OutputDir>` is `AnimusForge.OutputDir` (`/azerothcore/var/animus-forge` in the container, `var/animus-forge/shared` on the
cluster machines per `cluster.toml`). Under it:

| Path | Contents |
|---|---|
| `runs/<stage>/` | the run: `config.yaml`, `spec.json`, `stage.json`, `metrics.csv`, `layouts.csv`, `eval.jsonl`, `eval.csv`, `eval_episodes.jsonl`, `eval_baseline.json`, `progress.json`, `finished.json`, `latest.pt`, `best.pt`, `best_rung<k>.pt`, `checkpoint_*.pt` (pruned to the newest), `tb/`, `events.log`, `camera/`, `videos/` |
| `archive/<stage>-<timestamp>/` | earlier runs moved aside by a fresh start (nothing is deleted) |
| `layouts/<stage>/` | the sim's layout manifests and `stage.json` |
| `models/` (`AnimusForge.ModelDir`) | exports |
| `fast/`, `bench/` | fast-run and benchmark output |

Logs: `env/dist/logs/Server.log`, `Errors.log`, `animus-learner.log` (rank k: `animus-learner.rank<k>.log`), the export log,
`tensorboard.log`. File formats: [reference/file-formats.md](reference/file-formats.md).

## 7.5 Test runs without the long build

- `forge run <stage> random 256` then `forge status`: checks that characters build, the arena spawns and the sim
  steps, with no learner. It says nothing about learnability; there are no scripted baselines (principle 14).
- `forge fast [stage ...]`: trains each stage for a fixed budget (`AnimusForge.Fast.Budget`, default 20,000,000 steps;
  `forge fast 30M` overrides) with `AnimusForge.Fast.Envs` envs (default 16) and the overlay `configs/fast.yaml`, into
  `<OutputDir>/fast/` (`FastProfile`, `ForgeConfig.cpp:709`). `convergence.patience=0` is passed so a stage trains its whole
  budget. Clean with `forge clean fast`.
- `forgectl test [--gpu]`: GTests plus the CPU pytest in the dev container (forgectl.md).

## 7.6 Benchmarking

`forge bench [stage]` times every `AnimusForge.Bench.Threads x Envs` pair with a local policy and then the best few with
the learner; results go to `<OutputDir>/bench/bench.json`; `forge bench apply` writes the winner into the configs.
`AnimusForge.Bench.AutoTune` runs `bench auto` on a machine with no benchmark of its CPU. Per-decision timing columns
in `forge status`: world (map update), sim (module work), learner wait; the reset stall warning
("Reset stall: ...") is logged by `WatchResets`.

## 7.7 After changing C++

Cancel the stage, rebuild, resume: `forge cancel`; `./forge.sh --build` (or `forgectl build [--cluster]`); `forge resume
<stage>`. A rebuild changes the cluster fingerprint (the `Animus/` source hash), so every machine must be rebuilt; see
cluster.md. If a run's layouts changed, `resume` is refused (`runs.resume_mismatch`) or warns with the changed blocks
(`stages.layout_changes`); a change in a block's meaning bumps the block's revision so seeding by name carries on.

## 7.8 Exporting models

`forge export <stage> [best|latest]` starts `animus.export`; progress in the export log; models land in
`AnimusForge.ModelDir`. Copy by hand. What a realm needs is outside this repository: [06-animus.md](06-animus.md).

## 7.9 Running the learner by hand

Set `AnimusForge.Learner.AutoStart = 0`; the sim then logs the command to run (`LearnerProcess::ManualCommand`):
`cd apps/forge/python && <python> -u -m animus.train --config configs/<stage>.yaml --socket <path> --run-name <stage>
--runs-dir <OutputDir>/runs --layouts-dir <OutputDir>/layouts [--resume] [--set key=value ...]`. The client retries until
the sim's socket appears (it is created when a plan starts). `--set` overrides yaml values; `--overlay` merges a yaml.
Changing Python files under a running learner can mix versions (modules are imported lazily).

## 7.10 Troubleshooting

| Symptom | Meaning (from the code) |
|---|---|
| "Waiting for a learner started by hand" | `Learner.AutoStart` is 0 or the auto-start failed (`Learner auto-start failed`); start it with the printed command |
| "The world ticks N ms, but AnimusForge.DecisionMs ... wants M ms: rebuild the worldserver" | the binary and the conf disagree on the tick (`AnimusForge.cpp:464`); rebuild |
| "Env N is on map M instance I, which no map task ticked" | the env has no player to keep its map awake (scheduler skipped it); logged once |
| "Synchronous query on sealed DatabasePool" / "Write ... dropped on sealed DatabasePool" | something read or wrote SQL after startup; the first is a stack trace to fix, the second is informational (once per kind) |
| a crash with "Fatal signal, stack:" in `docker logs` | `FatalSignalHandler` printed frames; the core is also dumped |
| "Cluster: refused the worker at ..." | fingerprint differs (source, protocol, field count/bytes, curriculum keys, decision timing): `forgectl conf-sync --check`, `forgectl cluster` |
| learner exits and the sim waits | `forge resume <stage>` restarts the learner from `latest.pt`; a cluster host restarts failed learners itself up to three times |
| a stage "did not finish" yet the next stage warns it seeds from it | a cancelled stage still seeds the next from its checkpoint; the warning says so |
| `forge pause` and workers keep running | by design today: pause each worker's console (`forgectl stage pause` does) |
| "Reset stall: ..." | episode resets are slowing the sim (`Animus::Stall`); see the status line's reset columns |

More: [forgectl.md](forgectl.md) "When something does not work", [deploy-gate.md](deploy-gate.md) "The failure table".

## 7.11 Changing the code

The rules a change must keep are [principles.md](principles.md). Differences from upstream are listed in
[reference/01-forge-core-delta.md](reference/01-forge-core-delta.md); read its merge-risk section before touching
`World`, `Map*`, `WorldSession` or the movement handlers. Do not add a switch for behaviour nobody uses; delete the code.
How to extend the curriculum (stages, blocks, encounters, rewards) is described in the `cpp-*` reference documents
(UNVERIFIED: the former chapter 7.9 was written for the first curriculum and was removed).
