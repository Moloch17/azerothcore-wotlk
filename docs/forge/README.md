# The forge: documentation map

The forge is a fork of AzerothCore (branch `forge` of `Moloch17/azerothcore-wotlk`) that runs as a headless,
faster-than-real-time simulator for training World of Warcraft 3.3.5a bots, plus a Python MAPPO learner that trains one
policy per class through a curriculum of stages. The sim is C++ under `src/server/game/Animus`, built into the
worldserver; the learner is Python under `apps/forge/python/animus`; `forgectl` and `forge.sh` operate it.

These documents are written so that a person can review and refactor the whole project without an assistant. Every claim
cites `path:line`; anything not verified is marked `UNVERIFIED`. The reference documents describe what the code does,
not what comments say, and each ends with its observed issues.

## Start here

| Document | What it is |
|---|---|
| [audit/SUMMARY.md](audit/SUMMARY.md) | the consolidated audit: ranked findings, a work order, and the questions the owner has to decide |
| [reference/README.md](reference/README.md) | the index of the reference documents, with a reading order for a review |
| [04-curriculum.md](04-curriculum.md) | the stages manual: what each of the 12 stages teaches |
| [01-overview.md](01-overview.md) | what the forge is, the pieces, the twelve stages, a glossary of the core ideas |
| [principles.md](principles.md) | the rules the project is built to, and the review checklist |
| [reference/00-architecture.md](reference/00-architecture.md) | the whole system: processes, tick, decision loop, observation/action path, lifecycle, cluster, threading |
| [reference/01-forge-core-delta.md](reference/01-forge-core-delta.md) | exactly how the fork differs from upstream AzerothCore, file by file, with merge risks |

## Operating it

| Document | What it is |
|---|---|
| [07-operations.md](07-operations.md) | first start, the console commands, running and stopping stages, run directories, troubleshooting |
| [forgectl.md](forgectl.md) | the command line for the cluster (status, stages, builds, conf sync, videos) |
| [cluster.md](cluster.md) | the machines, ports, how code and runs move between them |
| [deploy-gate.md](deploy-gate.md) | the ordered checks before a build goes to the cluster |
| [decisions/0001-control-socket.md](decisions/0001-control-socket.md) | proposal to replace console typing with a control socket |
| [06-animus.md](06-animus.md) | the boundary with the separate mod-animus realm module (not in this tree) |
| [02-forge-core.md](02-forge-core.md) | short pointer from the old "forge core" chapter to the delta document |

## Reference (C++ sim)

| Document | What it is |
|---|---|
| [reference/cpp-runtime.md](reference/cpp-runtime.md) | env pool, bots, bridge, console and learner-process code in depth |
| [reference/cpp-movement.md](reference/cpp-movement.md) | the player controller and its link to the server |
| [reference/cpp-vision.md](reference/cpp-vision.md) | the camera, mental map, entity memory, GPU renderer, evaluation videos |
| [reference/cpp-blocks.md](reference/cpp-blocks.md) | observation/action blocks |
| [reference/cpp-layout-character.md](reference/cpp-layout-character.md) | layouts, the seat encoder, character building (class, talents, gear) |
| [reference/cpp-rewards-routing.md](reference/cpp-rewards-routing.md) | the reward ledger, route planning and fields |
| [reference/cpp-encounters.md](reference/cpp-encounters.md) | encounters: seek, interact, combat, party, roles, dungeons, ladders |
| [reference/cpp-stagescenario.md](reference/cpp-stagescenario.md) | `StageScenario` and the stage definitions |
| [reference/cpp-tuning-keys.md](reference/cpp-tuning-keys.md) | the `AnimusForge.Curriculum.*` tuning values |

## Reference (Python learner, data, tests)

| Document | What it is |
|---|---|
| [reference/py-learner.md](reference/py-learner.md) | training loop, evaluation, convergence, seeding, export, run files |
| [reference/py-mappo.md](reference/py-mappo.md) | networks, trainer, buffer |
| [reference/py-human-and-misc.md](reference/py-human-and-misc.md) | the human-capture tools and smaller modules |
| [reference/protocol.md](reference/protocol.md) | the wire protocol, version 25 |
| [reference/file-formats.md](reference/file-formats.md) | `.amdl`, `stage.json`, manifests, run files |
| [reference/metrics.md](reference/metrics.md) | metric and episode-info column names |
| [reference/config-keys.md](reference/config-keys.md) | every `Forge.*` and `AnimusForge.*` key |
| [reference/config-yaml.md](reference/config-yaml.md) | the learner's per-stage yaml |
| [reference/tests.md](reference/tests.md) | there are no tests (removed 2026-10-07), and the safety nets that went with them |
| [reference/tools-and-ops.md](reference/tools-and-ops.md) | `apps/forge/tools`, forgectl internals, scripts |
| [reference/stages.md](reference/stages.md) | the twelve stages, one section each |
| [reference/known-issues.md](reference/known-issues.md) | the collected list of bugs, debts and dead ends |
| [reference/glossary.md](reference/glossary.md) | terms |

## Removed chapters

The old chapters 03 (animus-lib), 05 (the learner and module) and 08 (reference) described the first curriculum (deleted
2026-10-07, git tags `curriculum-v1` and `pre-cleanup-2026-10-07`) and the time when the sim was a separate module.
The reference documents above replace them; they remain in git history
(`git show pre-cleanup-2026-10-07:docs/forge/05-animus-forge.md`).

## Repository layout (verified 2026-10-07)

| Path | Contents |
|---|---|
| `src/server/apps/worldserver/ForgeMain.cpp` | the only `main()` |
| `src/server/game/Forge/` | `ForgeCore` (playtest flag, `HasClients`, tick override) |
| `src/server/game/Animus/` | the sim: env pool, bots, bridge, scenarios, blocks, encounters, movement, vision, GPU |
| `src/server/scripts/Commands/cs_forge.cpp` | the `forge` console commands |
| `apps/forge/python/` | the learner (`animus/`), per-stage yaml (`configs/`) |
| `apps/forge/forgectl/`, `./forgectl` | the cluster command line |
| `apps/forge/tools/`, `apps/forge/patches/` | helper scripts, obsolete patches for the realm module |
| `apps/forge/cluster.toml` | the machines |
| `forge.sh`, `docker-compose*.yml`, `apps/docker/forge-worldserver.sh` | running it in containers |
