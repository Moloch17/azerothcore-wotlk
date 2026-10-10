# Python learner: `config.py`

Part of [py-learner.md](py-learner.md). Covers `apps/forge/python/animus/config.py` (853 lines), commit `bd32b9dc8`.
The yaml files themselves are in [config-yaml.md](config-yaml.md); the sim's conf keys in
[config-keys.md](config-keys.md). `MappoConfig` (the `mappo` section) lives in `animus/mappo/trainer.py:42-208` and is
documented in [py-mappo.md](py-mappo.md); it is not repeated here.

## Loading (`TrainConfig.load`, `config.py:719`)

1. `load_yaml(path)` (`:758`): `yaml.safe_load`; a top-level `extends: other.yaml` (relative to the file) is merged
   under it, recursively; a file extending itself (a cycle back to a path already seen) raises.
2. Each `--overlay` file is `merge`d over the whole result.
3. Each `--set key=value` is applied (`apply_override`, `:791`): dotted keys create sections, the value is
   `yaml.safe_load`ed
   (`total_env_steps=3e8` becomes a string and fails type checking later).
4. `from_dict(TrainConfig, raw)` (`:829`).

`merge(base, override)` (`:771`): dict values merge key by key if both sides are non-empty dicts; an empty dict replaces
(clears); `null` removes the key (so the field falls back to its default); anything else replaces.

`from_dict`: unknown keys raise `ValueError("unknown config keys: [...]")` (prefixed by section path); a value whose
type
does not fit the field's hint raises (`_matches`: ints fit floats, bools are never numbers, `tuple[...]`/`list[...]`
accept lists, element types are checked loosely); a list for a tuple field becomes a tuple; an int for a float field
becomes
a float. A section given as `null` becomes all defaults. `__post_init__` of each dataclass then validates. Dropping
unknown
`MappoConfig` keys applies only to a checkpoint's saved config in `evaluate.mappo_from_checkpoint`
(`evaluate.py:37`); a yaml stays strict.

Resolution helpers on `TrainConfig`: `local()` (`:592`, this learner's index and count among those sharing its sim),
`shared_runs` (`<runs_dir>/../../shared/runs`, assumes runs live in `<output>/<class>/runs`), `format_path`,
`resolved_init_from`, `resolved_merge_from`, `resolved_finetune_from`, `named_teachers`, `resolved_train_device`,
`resolved_rollout_device` (`resolve_device`, `:736`: `auto` is `cuda:0` if torch sees a GPU else `cpu`; a missing GPU
index falls back to `cuda:0`, no GPU to `cpu`, each with a line).

## `TrainConfig` fields (`:566-668`)

| Field | Default | Meaning / validation |
|---|---|---|
| `run_name` | `run` | overridden by `--run-name` (the scenario) |
| `runs_dir`, `layouts_dir` | `runs`, `layouts` | overridden by the sim |
| `socket` | `/tmp/animus-forge.sock` | Unix path or `tcp://host:port` |
| `cluster_sims` | `[]` | worker sims joined as one pool (`ClusterEnv`) |
| `cluster_timeout` | 60.0 | seconds a worker sim may stall before it sits out |
| `rank`, `ranks` | 0, 1 | global learner rank and count |
| `dist_address` | `127.0.0.1:29500` | where rank 0 meets the others |
| `local_rank`, `local_ranks` | -1, 0 | this machine's index/count (-1/0 = same as rank/ranks) |
| `dist_iface`, `dist_timeout` | "", 300.0 | gloo interface; collective timeout |
| `seed` | 1 | `seed_everything(seed + rank)` |
| `total_env_steps` | 5,000,000 | ceiling, "decisions x envs x agents" |
| `rollout_length` | 128 | decisions per rollout |
| `log_every` | 1 | updates between metrics rows |
| `checkpoint_every` | 25 | updates; also `checkpoint_env_steps` |
| `checkpoint_env_steps` | 5,000,000 | 0 = off |
| `keep_checkpoints` | 5 | numbered files kept; 0 = all |
| `overlap_updates` | False | update on a worker thread (acts on one-update-stale weights) |
| `train_device` | `auto` | |
| `rollout_device` | `cpu` | |
| `torch_threads` | 0 | 0 = torch default |
| `init_from` | `auto` | `auto`, a path or a list |
| `seed_from` | `latest` | `best` or `latest`, validated in `__post_init__` (`:670`) |
| `mappo.greedy_env_fraction` | 0.0 | exploration v3; `0 <= f <= 0.5` (validated in `TrainConfig.__post_init__`); this share of the learner's training envs act with the argmax of the movement heads and the free look (goals sampled), their movement policy-gradient terms masked ([py-mappo-trainer.md](py-mappo-trainer.md)); `round(f x E) == 0` for a nonzero `f` is refused at start. `move2_seek.yaml` 0.12; M3 and M4 restate 0.0 |
| `mappo.goal_cell_hindsight_lookback` | 0 | validated in `TrainConfig.__post_init__`: a whole number from 0 (off) to `MAX_HINDSIGHT_LOOKBACK` (256) decisions, and more than 0 only with `mappo.goal_cell_hindsight_coef > 0` (a window with no coefficient trains nothing, refused). The rule is in [py-mappo-trainer.md](py-mappo-trainer.md) |
| `merge_from` | `auto` | |
| `finetune_from` | `{runs_dir}/_finetune/{run_name}/best.pt` | used if the file exists |
| sections | | `mappo, distill, eval, convergence, layout_sampling, entropy_floor, cast, fade, costs, style, status` |

## `EvalConfig` (`:58`), section `eval`

| Field | Default | Notes |
|---|---|---|
| `every_env_steps` | 0 | 0 = never evaluate (then a stage trains to its budget) |
| `at_start` | True | evaluate seeded/fresh networks before training (only if no history) |
| `episodes` | 1024 | seeded episodes per evaluation |
| `seed` | 1000 | seed base |
| `deterministic` | True | argmax |
| `baseline` | "" | `"random"` or "" (validated) |
| `report` | `REPORT_COLUMNS` (`:28`) | episode columns printed and kept in summaries |
| `trace_episodes` | 0 | decisions of the first N seeds to `eval_trace.jsonl` |
| `sampled_every` | 0 | every N-th evaluation also scored with sampled actions |
| `mask_actions` | () | names forbidden during evaluation, resolved per layout |
| `heldout` | {} | `{arena: episodes}`; arena must be `eval_only` in stage.json |
| `heldout_every` | 4 | >= 1 |
| `heldout_on_best` | True | |
| `heldout_cadence` | {} | exploration v3; `{arena: N}`, the arena is played on every N-th evaluation only (always on the stage's last: `final`, and the ADVANCE re-run plays any it skipped); each key must be in `heldout`, each N a whole number >= 1 (`EvalConfig.__post_init__`); `move2_seek.yaml` `{sweep_rotating: 2}` (the rotation moves on by the evaluation number either way). The evaluation number is the leader's `len(tracker.history)`, broadcast to every rank |
| `score` | `outcome` | `outcome` -> `score_outcome` column, `return` -> whole return; checked lazily in `score_column()` (`:131`), not in `__post_init__` |
| `arms` | {} | names in `EVAL_ARMS = (with_human, with_partners, no_flag, no_camera, no_compass, no_map, no_memory, no_goal, random_goal, random_cell, no_plan, no_searched)`, int episodes >= 0; `no_searched` zeroes the crop's seventh channel and the map block's `searched`, `new_age`, `total` scalars (`searched_columns`, by the manifest's `map.scalar_names`); `no_goal` zeroes the goal block's columns (`ablation_chooser`, block `goal`), `random_goal` draws the goals uniformly over those on offer (`MappoTrainer.uniform_goals`), `random_cell` draws every cell goal's cell uniformly over the choosable blocks (`uniform_cells`), `no_plan` holds nothing beside the primary goal (`single_goal`); the last two are skipped where the stage has no cell goals |
| `arms_every` | 1 | >= 1 |
| `keep_motion_files` | 12 | >= 0 (int); evaluations whose `eval_motion_<env_steps>*.npz` route files and `eval_trace_<env_steps>_*.npz` trace files are kept (`realism.prune_routes`, counted over both kinds), 0 = none written and no held-out motion |
| `trace_all` | False | exploration v3; every seed of every evaluation set (plain, `learner_sampled`, each held-out arena, each arm) traced into `eval_trace_<env_steps>_<policy>.npz` ([file-formats.md](file-formats.md)), and the sampled and arm sets' routes written beside it (`eval_motion_<env_steps>_learner_sampled.npz`, `_<arm>.npz`); needs `keep_motion_files > 0`; `trace_episodes`' JSONL is independent of it |

## `ConvergenceConfig` (`:137`), section `convergence`

`advance` True; `patience` 0; `window` 4; `z` 2.0; `min_improvement` 0.02; `min_improvement_abs` 0.01; `kl` 0.003;
`entropy_slope` 0.01; `hold_share` 0.02; `lr_hold_until_plateau` True; `top_rung` True; `measure` "". No validation.
Meaning: [py-learner-stage.md](py-learner-stage.md). Every live yaml sets `patience: 3` through the chain
(`move1_controls.yaml:150`, `combat1_fight.yaml:126`); the sim's fast and bench profiles override it to 0.

## `FadeConfig` (`:176`), section `fade`

`enabled` False; `rungs` `(1.0, 0.5, 0.25, 0.0)`; `window` 3; `regress_z` 2.0; `give_up` 2; `moving_classes` 2;
`gate_metric` ""; `gate_value` 0.0; `require_plateau` True; `stall_evals` 4; `stall_env_steps` 20,000,000.
Validation (`:210`): rungs non-empty, within [0,1], strictly falling, last exactly 0.0; `window >= 1`; `regress_z > 0`;
`give_up >= 1`; `moving_classes >= 0`; `stall_evals >= 1`; `stall_env_steps >= 0`. Live yamls: all use
`[1.0, 0.5, 0.25, 0.0]` with `require_plateau: false` and a gate (for example M1 `arrived_at_rung >= 0.85`, `found >= 0.8`, `won >= 0.7`, `full_clear >= 0.5`).

## `CostLadderConfig` (`:230`), section `costs`

`enabled` False; `rungs` `(0.0, 0.25, 0.5, 1.0, 0.5, 0.25, 0.0)`; `window` 3; `regress_z` 2.0; `give_up` 2;
`gate_metric` "";
`gate_value` 0.0. Validation: rungs non-empty within [0,1], consecutive rungs differ; window, regress_z, give_up as
above.
No `require_plateau`, `moving_classes`, `stall_*`. Disabled in every live yaml (`enabled: false` in the per-stage
`costs` blocks that were checked).

## `StyleConfig` (`:271`), section `style` (see py-human-and-misc.md)

`enabled` False; `dataset` ""; `reference` ""; `coef` 0.02; `lr` 1e-4; `hidden` (256,256); `grad_penalty` 10.0;
`window` `human.motion.WINDOW`; `minibatches` 4; `batch` 512; `ladder` True; `eval_windows` 200000. Validation: dataset
required when enabled, coef >= 0, lr > 0, hidden non-empty positive, grad_penalty >= 0, window >= 2, minibatches and
batch
>= 1, eval_windows >= 0. Live yamls: `enabled: false`.

## `EntropyFloorConfig` (`:341`), `DistillConfig` (`:357`)

`entropy_floor`: `fraction` 0.0 (off), `max_boost` 4.0, `rate` 0.05; no validation (live: fraction 0.3 in move1).
`distill`: `teachers` "" (str or dict), `coef` 1.0, `half_life_env_steps` 30,000,000, `min_coef` 0.0. `named_teachers()`
raises
for a string other than "" and "auto". Not set in any live yaml; unusable with cameras
([py-learner-seeding.md](py-learner-seeding.md)).

## `PartnerConfig` (`:379`) and `CastConfig` (`:470`), section `cast`

`cast`: `agents` {}, `deterministic` False, `partners`. `partners`: `stages` (), `paths` (), `snapshot_every_env_steps`
0,
`newest_share` 0.0, `share` 0.0, `max_partners` 1, `pool_size` 8, `keep_newest` 2, `rate_window` 200, `floor` 0.1,
`score`
`score_outcome`, `deterministic` False, `eval_partners` (), `resident_members` 0. Validation (`:420`): `resident_members
>= 0`;
`share` and `newest_share` in [0,1]; a name with a colon (not a path) must end `:best` or `:latest`; `max_partners >=
1`;
`0 < resident_members < max_partners` refused. `enabled` = `share > 0` and some source. Live: `group1_roles` (stages
[combat3_survive], share 0.3, snapshots every 20M), later stages the previous stage with the default share 0 (stand-in
only
and arms).

## `StatusConfig` (`:489`), section `status`

`headline` (), `targets` {}, `excluded` {}. Validation: each target metric is in `headline` and matches
`^(>=|<=)\s*number$`; each excluded reason is non-empty plain text without `;` or `=`. `target_text()` and
`excluded_text()` give the flat strings in progress.json for the sim's console; `excluded_line()` is the evaluation
report
line. Targets are a readout, never a gate. `death_knight` is the excluded class in group and dungeon stages.

## `LayoutSamplingConfig` (`:535`), section `layout_sampling`

`enabled` False; `strength` 1.0; `max_ratio` 3.0; `metric` ""; `replay_fraction` 0.0; `role_metrics` {}. No validation.
Used by `send_layout_weights` and `send_replay` (`train.py:1492-1541`). Live yamls enable it with the stage's gate
metric
as `metric`, and `replay_fraction: 0.2` in M1.

## Observed issues

- `config.py:596`: the `seed` field follows a method definition inside the dataclass body.
- `config.py:67`, `:556`: comments cite `target.min_layout_episodes` and "confirmation seeds", which no longer exist.
- `config.py:149`, `:197`, `:613`, `:679` mention v1 stage names (`stage9_deadmines`, `stage3_rotation`, `stage8_duel`).
- `eval.score` is validated only when `score_column()` is first called, not at load.
- `ConvergenceConfig` has no validation (a negative window or patience loads).
- `config.py:18`: `config` imports `animus.human.motion` (for `WINDOW`), so loading a config imports the human package.
- `shared_runs` and `archive_run` (`runs.py:261`) assume `runs_dir.parent.parent` is the output directory.
- `layout_sampling.role_metrics` comment (`:558-562`) refers to stage6.
- `cast.partners.stages` comment (`:391-395`) mentions "the convergence bug" of old best.pt files.
