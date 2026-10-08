# Python learner: training machinery (index)

Purpose and scope: reference for the top-level modules of `apps/forge/python/animus/` that run a training stage: the
loop
(`train.py`), the stage convergence rule (`stage.py`), configuration, evaluation, seeding, cast and partners,
distillation,
the wire client, run-directory writers, export to `.amdl`, and the small tools. Written for a reviewer refactoring
without
an LLM. Facts are from commit `bd32b9dc8`; `UNVERIFIED:` marks what was not checked. Not covered here: `animus/mappo/*`
([py-mappo.md](py-mappo.md)), `animus/human/*`, `async_sync.py`, `parallel.py`, `style.py`
([py-human-and-misc.md](py-human-and-misc.md)).

Other docs: [00-architecture.md](00-architecture.md), [protocol.md](protocol.md), [file-formats.md](file-formats.md),
[metrics.md](metrics.md), [config-keys.md](config-keys.md), [config-yaml.md](config-yaml.md), [tests.md](tests.md),
[tools-and-ops.md](tools-and-ops.md), [stages.md](stages.md), [known-issues.md](known-issues.md),
[glossary.md](glossary.md), [cpp-stagescenario.md](cpp-stagescenario.md), [cpp-runtime.md](cpp-runtime.md).

## Sub-documents

| File | Content |
|---|---|
| [py-learner-train.md](py-learner-train.md) | argv and the console contract, run-directory writers, `TrainingRun` step by step (init, rollout, update, evaluation, checkpoint, finish), the lock-step protocol client |
| [py-learner-stage.md](py-learner-stage.md) | `ConvergenceTracker`, per-class convergence, `ConvergenceController`, rebaseline, shaping and cost ladders, alarms, lr/entropy schedules, restore |
| [py-learner-config.md](py-learner-config.md) | every config dataclass, field, default and validation; yaml loading and merging |
| [py-learner-seeding.md](py-learner-seeding.md) | seeding by name, merges, restricted stages, cast, partner pool, distillation, `stages.py` |
| [py-learner-export-tools.md](py-learner-export-tools.md) | export to `.amdl`, `evaluate`, `bench_learner`, runs, progress, episode means, rewards, lazy imports |

## Map table

| Path (under `apps/forge/python/animus/`) | Lines | Role |
|---|---|---|
| `__init__.py` | 7 | sets `HSA_ENABLE_IPC_MODE_LEGACY=0` before torch loads |
| `train.py` | 2309 | `TrainingRun`, `RunLogger`, `EvalLog`, checkpoints, the CLI entry point |
| `stage.py` | 829 | `ConvergenceController`, `LayoutState`, `ShapingFade`, `CostLadder`, `restore_evaluation_state` |
| `config.py` | 853 | `TrainConfig` and nested config dataclasses, yaml `extends`/overlay/`--set` loading |
| `evaluation.py` | 750 | seeded evaluation (`run_evaluation`), `EvalResult.summary`, `ConvergenceTracker`, layout weights |
| `episode_means.py` | 108 | per-event-weighted means of episode info columns |
| `bootstrap.py` | 903 | seeding networks from a parent checkpoint by block and name; merges |
| `export.py` | 900 | actor to `.amdl` models, reference reader and forward pass |
| `cast.py` | 326 | frozen-checkpoint actor, device residency, `Cast` facade |
| `partners.py` | 456 | co-op partner pool, draw, stand-in rows, evaluation chooser |
| `distill.py` | 456 | teacher construction and the KL auxiliary loss |
| `protocol.py` | 558 | wire structs, `Spec`, `Step`, encode/decode |
| `env.py` | 515 | `ForgeEnv` and `ClusterEnv` socket clients |
| `device.py` | 111 | HIP IPC device buffers shared with the sim; `host()` |
| `blas.py` | 78 | rocBLAS/TunableOp setup for gfx12 |
| `progress.py` | 165 | `progress.json` writer for the sim's console |
| `runs.py` | 100 | archive, prune, resume spec check, rung-best copies |
| `stages.py` | 115 | stage.json reader and layout-signature helpers |
| `rewards.py` | 88 | reward-mix audit (print-only) |
| `evaluate.py` | 150 | CLI: score a checkpoint on seeded episodes |
| `bench_learner.py` | 215 | CLI: time the learner against a fake sim |
| `async_sync.py`, `parallel.py`, `style.py` | 450, 204, 341 | not mine: see py-human-and-misc.md |

Directories `mappo/` and `human/` are other agents'.

## Console to learner (summary)

The sim starts `python -u -m animus.train --config ... --socket ... --run-name <scenario> --runs-dir ... --layouts-dir
...
[--resume]` plus `--set` pairs for threads, cluster sims, ranks and devices, then the operator's
`AnimusForge.Learner.Args`
(`LearnerProcess.cpp:80-155`). Details and the config resolution rule in [py-learner-train.md](py-learner-train.md).

## What happens, in one paragraph

Connect, read SPEC and stage.json, build networks, resume or seed by name, evaluate once, then loop: collect one rollout
(`rollout_length` decisions across all envs, acting group by group while the sim ticks the other group), compute
advantages, update (optionally on a worker thread), log a row, checkpoint on a clock. Every `eval.every_env_steps` run a
seeded evaluation: the controller observes it (best.pt, ladders, per-class convergence), converged classes leave the
training draw, layout weights and replay seeds go to the sim. The stage ends when every class played has converged and
the
ladders are settled, or at `total_env_steps`; then `latest.pt` and `finished.json` are written and the process exits 0.

## Cross-cutting observed issues (the file-specific ones are in the sub-documents)

1. `convergence.patience = 0` does not by itself force a full budget (the stage-ending rule never reads the overall
   tracker); the sim's comments say it does. UNVERIFIED by test.
2. Distillation (`distill.py`) refuses camera teachers, export (`export.py`) refuses camera checkpoints, and every live
   stage has a camera: both are unusable in the live curriculum. (`Distiller.__call__`, `.kl`, `.begin_sequence` were dead and are deleted.)
3. The cost ladder can never be gate-stepped, so it has no re-baselining, rung archives or alarms.
4. `init_from_checkpoint` replaces an explicit `best.pt` by `latest.pt` by default (tested, intentional), while two
   config comments say otherwise.
5. The resume check compares only six spec keys (`runs.RESUME_SPEC_KEYS`); episode info names, image/map bytes, look
   heads
   and every config value are not compared by the learner.
6. Checkpoints do not save the partner pool, cast state, layout weights or replay seeds.
7. A run that raises still saves `latest.pt` in `finish()`.
8. `eval.csv` rows for heldout, sampled, baseline and arm evaluations carry the learner tracker's margin and best.
9. `EvalResult.episodes` counts agent rows, not episodes.
10. Stale v1 stage names and comments in `train.py`, `config.py`, `export.py`, `rewards.py`, `bench_learner.py`
    (`grep -n "stage[0-9]_" animus/*.py`), and two references to modules that do not exist (`animus.gates`,
    `target.min_layout_episodes`).
11. `test_distill.py` is an empty file; `device.py`, `blas.py`, `bench_learner.py`, `evaluate.main` have no tests.
12. Lazy imports can mix versions if source changes under a running learner (list in
    [py-learner-export-tools.md](py-learner-export-tools.md)).

## Questions for the owner

- Is `convergence.patience = 0` meant to make a budget a budget? If so the stage-end rule needs the overall tracker.
- Should the cost ladder support `require_plateau` (gate stepping), or is `costs` retired?
- Are distillation and `.amdl` export to be revived for camera policies, or deleted?
- Should the partner pool's member scores be saved in checkpoints?
- Should `evaluate.py` send goals like the training evaluation?

## Coverage check

All 25 top-level `animus/*.py` files appear in the map table above (21 mine plus the four not mine). Command:
`ls apps/forge/python/animus/*.py | xargs -n1 basename` compared with this table.
