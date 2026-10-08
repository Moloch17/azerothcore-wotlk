# Python learner: export, evaluate, bench, and the small modules

Part of [py-learner.md](py-learner.md). Covers `export.py` (900 lines), `evaluate.py` (150), `bench_learner.py` (215),
`runs.py` (100), `stages.py` (115, see also [py-learner-seeding.md](py-learner-seeding.md)), `progress.py` (165),
`episode_means.py` (108), `rewards.py` (88), `device.py` (111), `blas.py` (78), `__init__.py` (7). Commit `bd32b9dc8`.

## `export.py`: checkpoint to `.amdl`

CLI: `python -m animus.export --checkpoint <ckpt> --out <dir> [--layouts-dir layouts]`. Loads the checkpoint, reads
`config.mappo.goal_every_decisions`, calls `export_layouts(actor_state, spec, out, <layouts_dir>/<scenario>,
goal_every)`
and prints each file. The sim's `forge export` starts this module.

`export_layouts` (`:424`): reads `stage.json` from the layouts directory on disk (not the checkpoint's saved `stage`),
names each model from stage.json `models` (`model_name`: stage name, else the scenario for one layout, else
`<scenario>_<layout>`), and writes `<out>/<model>.amdl` atomically (partial file, `os.replace`) plus a copy of
`<model>.json` from the manifest directory if present.

**It refuses cameras.** If any layout's stage.json blocks include `vision`, or the actor state has keys starting
`vision.` or `look_head.`, it raises `ValueError` before writing anything (`:440-450`): the camera and free look have no
`.amdl` section ("the realm format is parked"). Every live stage has a vision block (checked in
`var/animus-forge/shared/runs/move1_controls/stage.json` and `move2_seek`: 10 of 10 layouts; the later stages require
the
vision block in `Stages.cpp:1100-1159`). So no live-stage checkpoint can be exported today. If stage.json is missing
from
the layouts directory, `stage = {}` and the camera check by block is skipped (only the weight-key check remains) and
`seat_set_weights` returns None, so a checkpoint with seat sets would be exported without them (see issues).

Per layout the model is: adapter with the layout's observation normalisation folded in (`fold_normalisation`:
`W/sd`, `b - (W/sd) mean`, `sd = sqrt(var + 1e-5)`, in float64; skipped if `count == 0`), one zero-weight agent-id
column
(`with_agent_column`, `num_agents = 1`), the trunk layers, the layout's head, then the optional sections. The
format (little-endian) is documented field by field in the module docstring (`export.py:8-96`): magic `AMDL`, version
(`AMDL_VERSION = 9`; readers accept `AMDL_VERSIONS = (7, 8, 9)`), name, obs_dim, num_agents, num_actions, layers (in,
out,
weights, bias), recurrent size and GRU weights, foresight outputs and feedback weights, slow GRU, goal kinds/targets/
`goal_every_decisions` and the goal head, embedding, lookahead, slots, scales, a `director_sets` byte (always 0), seat
sets
(version 8; embed, sets, encoders, optional attention in version 9, pool weights, pointers). Every layer but the last is
followed by tanh. The GRU's state feeds the action head. Goals: chosen every `goal_every_decisions` or when the goal
block's ended column is set. The module docstring says the format "must be followed exactly and a change bumps
`AMDL_VERSION`".

`read_amdl` and `reference_decide` (numpy forward pass incl. goals, attention, pointers) are the reference for the C++
loader, which is not in this repository (it lives in the mod-animus module; UNVERIFIED location). Tests:
`test_export.py`
(7), `test_export_seat_sets.py`.

## `evaluate.py`: score a checkpoint against a running sim

`python -m animus.evaluate --checkpoint <ckpt> [--episodes 128] [--seed 1000] [--baseline random] [--socket P]
[--stochastic] [--episodes-file PATH] [--mask-actions NAME ...]`. The sim must run the checkpoint's scenario with
`AnimusForge.Policy = "remote"` and `Learner.AutoStart = 0`.

Flow: load the checkpoint; `mappo_from_checkpoint` drops config keys this build's `MappoConfig` no longer has (one
printed
line) and rebuilds it with `hidden` as a tuple; connect `ForgeEnv(socket)`; refuse on scenario mismatch,
`resume_mismatch`, or `layout_changes`; check the camera bytes and look heads; build `MappoTrainer(layouts, state_dim,
mappo, vision)`; `load_state_dict(..., load_optimizers=False)`; resolve `--mask-actions` (default: the
checkpoint's `eval.mask_actions`); `env.reset()`; optionally score the baseline; `run_evaluation` with a chooser that
clears the acting state on done, applies the mask, calls `trainer.act` (argmax unless `--stochastic`) and returns the
look when there is one; prints the score and `format_summary`.

Differences from the training evaluation: the chooser does not send goals (it returns `chosen` or `(chosen, None,
look)`),
unlike `TrainingRun._acting` which sends `wire_goals`; it scores `SCORE_COLUMN` whatever the checkpoint's `eval.score`
was; `MappoTrainer` is built with its default devices (UNVERIFIED which); it does not use cast or partners. The goals
are
described as "scored and reported by the sim; they mask nothing" (`env.py:87-89`), UNVERIFIED how much the missing goals
change a goal-head policy's score. Tests: `test_config_unknown_keys.py` covers `mappo_from_checkpoint` only.

## `bench_learner.py`: time the loop against a fake sim

`python -m animus.bench_learner --config <yaml> --spec <run>/spec.json --layouts <dir> [--sim-ms 5] [--half-batch]
[--updates 6] [--envs N] [--set K=V]`. Starts `fake_sim` in a spawned process on a temporary Unix socket: it speaks the
real protocol, sends random observations (eight reused frames), random bytes for camera rows, episodes of the stage's
length, and sleeps `--sim-ms` per decision (per half with half-batch). The real `TrainingRun` trains against it with
overrides: no init/merge/distill, no evaluation, a huge budget, `checkpoint_every=1000000`, `convergence.patience=0`,
`run_name=bench`. Prints per-update rollout and update seconds, the median env steps per second after the first update,
and peak device memory. `spec_from_dict` rebuilds a `Spec` from `spec.json` or a checkpoint's `spec` and forces the
current protocol version; `tools/resume_check.py` also imports it. Nothing about learning is measured.

## `runs.py`: run directories

`ARCHIVE_DIR = "archive"`, `CHECKPOINT_GLOB`, `LATEST_CHECKPOINT`, `FINISHED_FILE = "finished.json"`,
`RESUME_SPEC_KEYS = (scenario, agents_per_env, obs_dim, state_dim, num_actions, layouts)`.
`prune_checkpoints(run_dir, keep)` deletes all but the newest `keep` `checkpoint_*.pt` (name order is update order).
`archive_run(run_dir)` moves a non-empty directory to `run_dir.parent.parent/archive/<run>-<time>[-n]` and recreates it.
`resume_checkpoint_path` requires `latest.pt`. `resume_mismatch(a, b)` lists the keys whose normalised (tuple to list)
values
differ. `rung_best_name(ladder, rung)` is `best_rung<k>.pt` for the fade, `best_<ladder>_rung<k>.pt` otherwise (the
second
form is unreachable, see py-learner-stage.md). `archive_rung_best` copies `best.pt` atomically. Tests: `test_runs.py`
(8).

## `progress.py`: `progress.json`

`write_progress(run_dir, fields, undefined)` writes a flat JSON object atomically (partial then replace). Booleans
become
ints; NaN and infinities become null and are named in `nonfinite` (a NaN in `undefined` is a by-design null). The sim
reads only top-level scalars. `ProgressWriter` keeps static fields (run name, scenario, budget, started_at, resumed
counters,
`eval_every`, `patience`, `window`, `baseline`, `status_headline`, `status_targets`, `status_excluded`), the latest
metrics
row, the evaluation block (`evals`, `last_eval_*`, `best_*`, `evals_since_best`, `converged_layouts`, `active_layouts`,
`weakest_layout`, `weakest_missing`, `reentries`, `eval_<headline metric>`), and the arm readings (`eval_<arm>_score`,
`eval_<metric>_<arm>`, and `ARM_SPLITS`: `clear_standin` against `clear_allbot` giving `standin_gap`). `write(phase,
...)`
merges them; phases written: `training`, `evaluating`, `finished`, `stopped`. `note(key, text)` stores a line (stand-in
status). Tests: `test_progress.py` (4), `test_status_headline.py`. Reader: the C++ console,
`src/server/game/Animus/Console/Progress.cpp` (the path in the docstring,
`src/Console/Progress.cpp`, is stale).

## `episode_means.py`, `rewards.py`

`episode_means.means(values, names)`: column means over episodes; a column in `PER_EVENT` (event column -> count column,
`:14-81`) is the count-weighted mean and NaN if no episode had the event; `undefined(...)` names those NaN columns so
they
are not reported as faults. The same table is applied inside `EvalResult.summary` (`evaluation.py:200-208`).
`rewards.py`: `audit(mix, outcome_terms, max_share)` returns the worst shaping term earning more than
`MAX_SHAPING_SHARE = 0.5` of the largest positive outcome term; `outcome_terms(stage)` takes terms of category outcome
or
cost from stage.json `reward_terms` (fallback `OUTCOME_TERMS = (kill, clear, arrive)`); `WARN_EVERY = 25`. Print-only.
Tests: `test_rewards.py`, `test_metric_names.py`, `test_outcome_score.py`.

## `device.py`, `blas.py`, `__init__.py`

See [py-learner-train.md](py-learner-train.md). `animus/__init__.py` sets `HSA_ENABLE_IPC_MODE_LEGACY=0` before torch
loads.

## Lazy imports (known fact: changing files under a running learner can mix versions)

The learner imports most modules at start, but these imports run later, at call time: `env.py:203`
(`from .device import open_buffers`), `export.py:365`, `:396` (they import `SEAT_SET_NAMES` and `EntitySets`, both deleted), `config.py:740`
(`import torch` in `resolve_device`), `mappo/trainer.py:2039` (`BLIND_KEEP_PREFIXES`), `protocol.py:253`, `:292`
(`torch` for device views), `bench_learner.py:155-156`, `blas.py:153`, `:164`, `:166`. A source file replaced while the
learner runs is read at the next such call, so a running learner can hold some modules of the old version and load
others
of the new one. Most are inside code paths that run at start (seeding) or only on a GPU; `env.py:203` runs once at
connect.

## Observed issues

- `export.py:224`, `:243`, `:253`: `name` (the model name parameter of `write_amdl`) is reused as a loop variable after
  the
  header is written. Harmless, but a trap.
- `export.py:437`: stage.json comes from the layouts directory only; a missing file silently drops seat sets and the
  camera
  check. The checkpoint carries `stage` and could be used.
- `export.py` cannot export any live stage (camera refusal).
- `export.py:3`: the example names `stage4_duel`.
- `evaluate.py:118-121`: goals are not sent; scoring uses `SCORE_COLUMN` regardless of `eval.score`.
- `bench_learner.py:3`: example `stage4_duel`; the `ForgeConfig.cpp` comment about a refusal of gated stages has no
  match.
- `progress.py:5`: stale path for the reader (`src/Console/Progress.cpp`).
- `rewards.py:1-47`: docstring is about the v1 curriculum.
