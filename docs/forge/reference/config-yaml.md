# The learner's yaml configuration

Purpose and scope. Every field of the learner's configuration (`TrainConfig` and its nested sections in
`apps/forge/python/animus/config.py`, plus `MappoConfig` in `animus/mappo/trainer.py`): type, default, validation and
where it is read; how a stage yaml inherits (`extends`, `null`, lists, maps); how the sim hands the yaml to the learner;
and the 10 live yamls compared field by field. The sim's own `AnimusForge.*` keys are in
[config-keys.md](config-keys.md). What the learner does with the values is in [py-learner.md](py-learner.md) and
[py-mappo.md](py-mappo.md); what the stages are, in [stages.md](stages.md). The suites that used to load every yaml
were removed ([tests.md](tests.md)).

## Map of the files in this area

| Path | Lines | Role |
|---|---|---|
| `apps/forge/python/animus/config.py` | 831 | the config dataclasses, `load_yaml`, `merge`, `apply_override`, `from_dict`, `resolve_device` |
| `apps/forge/python/animus/mappo/trainer.py` | (lines 41-198) | `MappoConfig`, the `mappo:` section |
| `apps/forge/python/configs/move1_controls.yaml` | 207 | root of the movement chain |
| `apps/forge/python/configs/move2_seek.yaml` | 126 | extends move1_controls |
| `apps/forge/python/configs/move3_interact.yaml` | 94 | extends move2_seek |
| `apps/forge/python/configs/move4_follow.yaml` | 93 | extends move2_seek |
| `apps/forge/python/configs/combat1_fight.yaml` | 172 | root of the combat/party/dungeon chain (extends nothing) |
| `apps/forge/python/configs/combat2_packs.yaml` | 60 | extends combat1_fight |
| `apps/forge/python/configs/combat3_survive.yaml` | 61 | extends combat2_packs |
| `apps/forge/python/configs/group1_roles.yaml` | 121 | extends combat3_survive |
| `apps/forge/python/configs/dungeon2_ragefire.yaml` | 109 | extends group1_roles |
| `apps/forge/python/configs/dungeon3_deadmines.yaml` | 72 | extends dungeon2_ragefire |
| `apps/forge/python/configs/fast.yaml` | 44 | overlay for `forge fast`; not a stage (never extended) |

(Line counts are physical lines, comments included; the 10 live files are 1,115 lines of yaml and comments.)

## How a config reaches the learner

1. The sim builds the learner command line in `LearnerArgs`
   (`src/server/game/Animus/Learner/LearnerProcess.cpp:80-154`):
   `<python> -u -m animus.train --config <LearnerConfigFor(scenario)> --socket <socket> --run-name <scenario>
   --runs-dir <OutputDir>/runs --layouts-dir <OutputDir>/layouts [--resume]`, then `--set` pairs the sim adds
   (`torch_threads`, `cluster_sims`, `rank`, `ranks`, `local_rank`, `local_ranks`, `dist_address`, `dist_iface`,
   `mappo.rank_sync`, `train_device`, `rollout_device`), then the operator's `AnimusForge.Learner.Args`
   (`--set` later wins). `forge fast` puts `--overlay <configs/fast.yaml>`, `--set total_env_steps=<budget>` and
   `--set convergence.patience=0` first in `ForgeConfig::LearnerArgs` (`ForgeConfig.cpp:709-735`), which
   `LearnerProcess` appends after its own `--set` pairs. `LearnerConfigFor` picks `configs/<scenario>.yaml`, or
   `configs/<class>/<scenario>.yaml` when `Classes` names one class and that file exists
   ([config-keys.md](config-keys.md)); there are no per-class directories now.
2. `animus/train.py:main` (`train.py:2271-2308`) calls `TrainConfig.load(args.config, args.set, args.overlay)`, then
   overwrites `socket`, `run_name`, `runs_dir`, `layouts_dir` from the command line when given, then `blas.prepare`
   and `TrainingRun(config, resume=...).run()`.
3. `TrainConfig.load` (`config.py:719-730`): `load_yaml(path)` (follows `extends`), merges each `--overlay` over the
   result, applies each `--set key=value` in order, then `from_dict` builds the dataclasses.

## Inheritance and merge rules

- `extends: <file>.yaml` must be the key `extends` of the file; its path is relative to the file that names it
  (`config.py:758-768`). The key is removed before merging, so an overlay may itself extend. A file that extends one
  already in the chain is an error ("extends itself"; the check covers the whole chain through `seen`).
- `merge(base, override)` (`config.py:771-788`), key by key:
  - `null` (YAML `null` or `~`) **removes the key** from the merged result. The field then takes its dataclass default,
    **not** the parent's value (the parent's is gone). Used to drop an inherited `status.targets` entry
    (`arrived: null` in move2_seek) and to un-set `costs.gate_metric` (`move4_follow`).
  - a non-empty map over a map merges recursively;
  - an empty map `{}` **replaces** (clears) the parent's map;
  - anything else, including every list (so `eval.report`, `status.headline`, `fade.rungs`, `mappo.hidden`,
    `cast.partners.stages` replace the parent's whole list), replaces.
- `--overlay` files are merged over the whole result the same way (their own `extends` followed), then `--set`.
- `--set a.b=value` (`apply_override`, `config.py:791-802`): creates intermediate sections with `setdefault`, the value
  is parsed by `yaml.safe_load` (so `3e8` is a string and fails the type check; write `300000000`), and it can set a key
  under a map-valued field (`status.targets.x=">= 1"`).
- `from_dict` (`config.py:829-853`): an unknown key is a `ValueError` naming it (`unknown config keys`); a value of the
  wrong type is an error naming the key; ints fit float fields, bools are never numbers, lists become tuples for tuple
  fields, nested dataclass sections recurse. **Strict for yaml; lenient only for a checkpoint's saved config**
  (`evaluate.py:mappo_from_checkpoint` drops and names removed `mappo` options: `evaluate.py:36-47`).
- Path-valued fields take `{runs_dir}`, `{run_name}` and (where `format_path` is used) `{shared_runs}`
  (`config.py:685-686`; `shared_runs` is `<runs_dir>/../../shared/runs`).
- Two roots exist: `move1_controls.yaml` and `combat1_fight.yaml` both carry a full config (97 and 100 flattened keys
  when the maps `targets`, `excluded`, `heldout`, `arms` are counted as one).
  The curriculum order across them is by seeding (`init_from: auto` follows the sim's `stage.json` seed chain), not by
  `extends`.

## Fields

Defaults are the dataclass defaults. "Validation" lists checks in `__post_init__` (a failed check is a `ValueError`
at load) beyond the type check. "Read by" is a heuristic from searching for `section.field` / `config.field`
references outside `config.py`; `-` means no direct reference was found by that search and the value is read through
the section object by the class that receives it (named in the section heading).

### Top level (`TrainConfig`, `config.py:567`)

| Field | Type | Default | Validation | Read by |
|---|---|---|---|---|
| `run_name` | str | `"run"` | | `train.py:561`, `progress.py:63`; the sim overrides with the scenario |
| `runs_dir` | str | `"runs"` | | `config.py:683-699`, `train.py`; the sim passes `<OutputDir>/runs` |
| `layouts_dir` | str | `"layouts"` | | `train.py:604, 2302` |
| `socket` | str | `/tmp/animus-forge.sock` | | `train.py:588-592, 2296`; `tcp://...` for a worker |
| `cluster_sims` | list[str] | `[]` | | `train.py:587` (set by the sim) |
| `cluster_timeout` | float | 60.0 | | `train.py:591` |
| `rank`, `ranks` | int | 0, 1 | | `async_sync.py`, `parallel.py`, `train.py` (set by the sim) |
| `dist_address` | str | `127.0.0.1:29500` | | `train.py:554, 725, 753` |
| `local_rank`, `local_ranks` | int | -1, 0 | | `config.py:592-595` (`local()`) |
| `dist_iface` | str | `""` | | `train.py:555` |
| `dist_timeout` | float | 300.0 | | `train.py:555, 725, 759, 1606` |
| `seed` | int | 1 | | `train.py:559, 926, 1067` |
| `total_env_steps` | int | 5,000,000 | | `progress.py:65`, `stage.py:527, 533`, `train.py:1737` (ceiling; a stage ends on convergence) |
| `rollout_length` | int | 128 | | `train.py:692, 769`, `bench_learner.py` |
| `log_every` | int | 1 | | `train.py:2018` (updates) |
| `checkpoint_every` | int | 25 | | `train.py:1138` (updates) |
| `checkpoint_env_steps` | int | 5,000,000 | | `train.py:1137` |
| `keep_checkpoints` | int | 5 | | `train.py:1144` (0 = all) |
| `overlap_updates` | bool | false | | `train.py:798, 805` (update on a worker thread, one update stale) |
| `train_device` | str | `auto` | resolved by `resolve_device` | `config.py:714`, `mappo/trainer.py:632` |
| `rollout_device` | str | `cpu` | resolved by `resolve_device` | `config.py:717`, `env.py:26, 214`, `mappo/trainer.py:638` |
| `torch_threads` | int | 0 | | `train.py:543` |
| `init_from` | str or list | `auto` | | `config.py:688-692`, `train.py:986`; `auto` = the stage seed chain's `best.pt` |
| `seed_from` | str | `latest` | `best` or `latest`, else error (`config.py:670-673`) | `train.py:982` |
| `merge_from` | str or list | `auto` | | `config.py:697-701`; each merged stage's `best.pt` |
| `finetune_from` | str | `{runs_dir}/_finetune/{run_name}/best.pt` | | `config.py:694-695`; seeds the run before `init_from` |

`resolve_device` (`config.py:736-755`) turns `auto` into `cuda:0` or `cpu`, and a missing `cuda:N` into `cuda:0`
(printing a line) or `cpu`; it imports torch.

### `eval:` (`EvalConfig`, `config.py:59`)

Read by `evaluation.py` (`run_evaluation`, line 451), `train.py` (1150-1310, 2229), `progress.py:69-83`, `stage.py`.

| Field | Type | Default | Validation / note |
|---|---|---|---|
| `every_env_steps` | int | 0 | 0 = no evaluation (stage then trains to `total_env_steps`) |
| `at_start` | bool | true | also evaluate the starting networks (`train.py:2229`) |
| `episodes` | int | 1024 | seeded episodes per evaluation |
| `seed` | int | 1000 | seed base |
| `deterministic` | bool | true | argmax actions |
| `baseline` | str | `""` | only `""` or `"random"` (`config.py:116-118`) |
| `report` | tuple[str] | `REPORT_COLUMNS` (`config.py:27-55`) | episode-info columns printed per band (`train.py:858`) |
| `trace_episodes` | int | 0 | decision traces to `eval_trace.jsonl` |
| `sampled_every` | int | 0 | every N evaluations also score sampled actions (`train.py:1303`) |
| `mask_actions` | tuple[str] | `()` | evaluation-only action mask by name; unknown name refused at startup (`evaluation.py:348-367`) |
| `heldout` | dict | `{}` | arena name to episodes, played apart, never steers (`train.py:301-310`) |
| `heldout_every` | int | 4 | at least 1 (`config.py:127-129`) |
| `heldout_on_best` | bool | true | also play on a new `best.pt` |
| `score` | str | `outcome` | `outcome` or `return` (checked in `score_column`, `config.py:131-134`, not at load) |
| `arms` | dict | `{}` | arm name to episodes; arms must be in `EVAL_ARMS = (with_human, with_partners, no_flag, no_camera, no_compass, no_map, no_memory)`, counts non-negative ints (`config.py:119-124`) |
| `arms_every` | int | 1 | at least 1 |
| `keep_motion_files` | int | 12 | 0 or more: how many evaluations' route files (`eval_motion_<env_steps>[_heldout_<arena>].npz`) are kept; 0 writes none and the held-out arenas collect no motion |

### `convergence:` (`ConvergenceConfig`, `config.py:138`), read by `stage.py` `ConvergenceController` (line 460) and
`evaluation.py` `ConvergenceTracker` (line 623)

| Field | Default | Note |
|---|---|---|
| `advance` | true | false: never ends on convergence (`stage.py:730`) |
| `patience` | 0 | evaluations without a new overall best before the score counts as plateaued (LR anneal); the sim sets 0 in `forge fast` |
| `window` | 4 | evaluations each class signal is read over |
| `z` | 2.0 | a new best must beat the best by z standard errors, or ... (`evaluation.py:626-650`) |
| `min_improvement` | 0.02 | ... this fraction of |best|, or ... |
| `min_improvement_abs` | 0.01 | ... this much, whichever is largest |
| `kl` | 0.003 | LR-normalised approx_kl under which a class has stopped moving (`stage.py:15`) |
| `entropy_slope` | 0.01 | per evaluation of entropy / ln(allowed actions) (`stage.py:19`) |
| `hold_share` | 0.02 | draw share of a converged class (`stage.py:28, 711`) |
| `lr_hold_until_plateau` | true | `stage.py:531` |
| `top_rung` | true | a ladder stage converges only at its top rung (`stage.py:23, 589`) |
| `measure` | `""` | the stage's own measure column the per-class plateau reads (`stage.py:26, 666`) |

### `fade:` (`FadeConfig`, `config.py:177`) and `costs:` (`CostLadderConfig`, `config.py:231`)

Read by `stage.py` (`ShapingFade`, line 136; `CostLadder`, line 382, a subclass) and built at `train.py:340-353`. Same
semantics: every Shaping term (fade) or noise price (costs) is paid times the current rung's scale.

| Field | fade default | costs default | Validation |
|---|---|---|---|
| `enabled` | false | false | |
| `rungs` | (1.0, 0.5, 0.25, 0.0) | (0.0, 0.25, 0.5, 1.0, 0.5, 0.25, 0.0) | fade: non-empty, within [0,1], strictly falling, ending at 0.0; costs: within [0,1], each unlike the one before |
| `window` | 3 | 3 | at least 1 |
| `regress_z` | 2.0 | 2.0 | more than 0 |
| `give_up` | 2 | 2 | at least 1 |
| `moving_classes` | 2 | (fade only) | 0 or more; `stage.py:680` |
| `gate_metric` | `""` | `""` | the step-down waits for this summary column (`stage.py:149-151, 260`) |
| `gate_value` | 0.0 | 0.0 | |
| `require_plateau` | true | (fade only) | false: a gate-stepped ladder steps as soon as the gate is met (`stage.py:571, 641`) |
| `stall_evals` | 4 | (fade only) | at least 1 (`config.py:223`); stall warning (`stage.py:174, 316`) |
| `stall_env_steps` | 20,000,000 | (fade only) | 0 or more |

### `style:` (`StyleConfig`, `config.py:272`), read by `style.py` and `train.py:908-934, 1338, 1779`

`enabled` false; `dataset` `""` (required when enabled: error otherwise); `reference` `""`; `coef` 0.02 (not negative);
`lr` 1e-4 (positive); `hidden` (256, 256) (all widths above 0); `grad_penalty` 10.0 (not negative); `window`
`MOTION_WINDOW` (imported from `animus.human.motion`; at least 2); `minibatches` 4, `batch` 512 (each at least 1);
`ladder` true; `eval_windows` 200,000 (0 or more). Every live yaml has `style.enabled: false` or inherits false.

### `entropy_floor:` (`EntropyFloorConfig`, `config.py:342`), read by `stage.py:544-555`

`fraction` 0.0 (0 = off), `max_boost` 4.0, `rate` 0.05. No validation.

### `distill:` (`DistillConfig`, `config.py:358`), read by `distill.py`, `train.py:1017`

`teachers` `""` (`""`, `"auto"` or `{arena: path}`; anything else raises in `named_teachers`, `config.py:703-711`),
`coef` 1.0, `half_life_env_steps` 30,000,000, `min_coef` 0.0. Set by no live yaml.

### `cast:` (`CastConfig`, `config.py:471`) and `cast.partners:` (`PartnerConfig`, `config.py:380`)

`cast.agents` dict (stage.json `cast` entries to checkpoint path), `cast.deterministic` false (no reference found).
`cast.partners` read by `partners.py` and `train.py:1032-1078, 1219-1225, 1464`:

| Field | Default | Validation / note |
|---|---|---|
| `stages` | `()` | earlier stages by name (`<stage>` = latest.pt else best.pt; `<stage>:best`, `<stage>:latest`) |
| `paths` | `()` | checkpoint paths |
| `snapshot_every_env_steps` | 0 | 0 = never |
| `newest_share` | 0.0 | within [0,1] |
| `share` | 0.0 | within [0,1]; partners are enabled only when `share > 0` and some member source exists (`enabled`, `config.py:439-441`) |
| `max_partners` | 1 | at least 1 |
| `pool_size`, `keep_newest` | 8, 2 | |
| `rate_window` | 200 | |
| `floor` | 0.1 | |
| `score` | `score_outcome` | |
| `deterministic` | false | |
| `eval_partners` | `()` | |
| `resident_members` | 0 | 0 or at least `max_partners` |

A name with a colon must end in `best` or `latest` (`config.py:426-431`).

### `status:` (`StatusConfig`, `config.py:490`), read by `progress.py:74-79`, `train.py:858, 1281`

`headline` (), `targets` {} (each key must be in `headline`; value `">= x"` or `"<= x"`), `excluded` {} (name to reason;
a
reason is non-empty plain text without `;` or `=`). Carried to the sim flat through `progress.json`
(`target_text`, `excluded_text`). Targets are a readout, never a gate.

### `layout_sampling:` (`LayoutSamplingConfig`, `config.py:536`), read by `evaluation.py:371-410` and
`train.py:1495-1520`

`enabled` false; `strength` 1.0; `max_ratio` 3.0; `metric` `""`; `replay_fraction` 0.0; `role_metrics` {}. No
validation.

### `mappo:` (`MappoConfig`, `mappo/trainer.py:42`); no validation beyond types; read by `mappo/trainer.py` and
`mappo/networks.py` ([py-mappo.md](py-mappo.md))

| Field | Default | Read by (first) |
|---|---|---|
| `hidden` | (128, 128) | `trainer.py:644` |
| `gamma`, `gae_lambda`, `reference_decision_ms` | 0.99, 0.95, 100 | `trainer.py:265-268` (`per_decision`: compounded to `AnimusForge.DecisionMs`) |
| `clip`, `value_clip` | 0.2, 0.2 | `trainer.py:1391, 1906` |
| `entropy_coef`, `look_entropy_coef` (None = `entropy_coef`) | 0.01, None | `trainer.py:639, 1176`; `stage.py:475, 526` |
| `vision_chunk_rows` | 0 (int or `auto`) | `trainer.py:1368, 1579`; `auto` is resolved once by `train.choose_vision_chunk_rows` (see py-mappo-trainer.md) |
| `value_coef` | 1.0 | `trainer.py:1397` |
| `actor_lr`, `critic_lr` | 5e-4, 5e-4 | `trainer.py:742-748` |
| `epochs`, `minibatches` | 5, 4 | `trainer.py:1980` |
| `max_grad_norm` | 0.5 | `trainer.py:1433` |
| `use_value_norm`, `value_norm_beta` | true, 0.99 | `trainer.py:676` |
| `per_layout_advantages`, `min_layout_rows` | true, 32 | `trainer.py:1553, 1558` |
| `target_kl` | 0.0 | `trainer.py:1971` |
| `normalise_observations` | true | `trainer.py:2013` |
| `lr_final_fraction`, `entropy_final_fraction` | 1.0, 1.0 | `stage.py:526, 532`, `train.py:1394` |
| `foresight_coef`, `foresight_horizons_seconds`, `foresight_time_scale_seconds` | 0.0, (5.0, 30.0), 60.0 | `trainer.py:647, 1799`; `train.py:685-686` |
| `foresight_obs_targets`, `foresight_feedback` | false, false | `trainer.py:646`; `networks.py:2029` |
| `recurrent_size`, `chunk_length` | 0, 0 | `train.py:697-699`; `trainer.py:1497, 1570` |
| `rollout_graphs` | true | `trainer.py:890, 903` |
| `rank_sync`, `weight_sync_every` | `gradients`, 1 | `parallel.py:117`, `train.py:546, 758`; the sim sets `mappo.rank_sync` for clusters |
| `goal_count`, `goal_targets`, `goal_every_decisions`, `goal_slots` | 0, 1, 16, 1 | `networks.py:2006`; `trainer.py:649-659` |
| `hindsight_coef` | 0.0 | `trainer.py:1639, 1818` |
| `goal_entropy_scale`, `goal_entropy_final_fraction`, `goal_slot_entropy_weight` | 1.0, 1.0, 0.1 | `train.py:1736-1737`; `trainer.py:665` |
| `slow_goal_size`, `slow_goal_gamma`, `slow_goal_lambda`, `slow_goal_lr` | 0, 0.993, 0.95, 3e-4 | `trainer.py:654-659, 746`; `train.py:1712` |
| `goal_lookahead`, `lookahead_coef` | false, 0.5 | `trainer.py:662, 196` |

## The 10 live yamls, field by field

Each row lists what a yaml sets that differs from its parent's effective value (parent in brackets), derived by reading
the
files. "Restated" means the key is present with the parent's value (no effect). Every live stage has `mappo.hidden =
[256, 512, 512]`, `recurrent_size 128`, `goal_count 12`, `goal_slots 4`, `goal_targets 29`, `slow_goal_size 128`,
`target_kl 0.02`, `hindsight_coef 0.1`, `style.enabled false`, `layout_sampling.enabled true`
(from the roots) unless stated.

Chain A (movement): `move1_controls` (root) <- `move2_seek` <- `move3_interact`, and `move2_seek` <- `move4_follow`.
Chain B: `combat1_fight` (root) <- `combat2_packs` <- `combat3_survive` <- `group1_roles` <- `dungeon2_ragefire` <-
`dungeon3_deadmines`.

| Yaml (parent) | Overrides |
|---|---|
| `move1_controls` (root) | `total_env_steps` 150M; `rollout_length` 128; `overlap_updates` true; `torch_threads` 4; `checkpoint_every` 100; `init_from` auto; `mappo`: gamma 0.997, gae_lambda 0.985, chunk_length 32, epochs 4, actor/critic lr 1.5e-4, lr_final_fraction 0.1, entropy_final_fraction 0.3, look_entropy_coef 0.001, vision_chunk_rows 0; `eval`: every 5M, episodes 512, sampled_every 3, trace_episodes 64, 37-name report; `status`: 15-name headline, 10 targets; `convergence.measure` arrived, patience 3; `fade`: enabled, `rungs [0.0]`, gate `arrived_at_rung` 0.8, window 3; `costs`: disabled, gate `arrived_at_rung` 0.8; `entropy_floor.fraction` 0.3; `layout_sampling`: strength 1.0, max_ratio 4.0, metric arrived, replay_fraction 0.2; `style.enabled` false |
| `move2_seek` (move1_controls) | `run_name`; `seed_from` latest (was default latest); `total_env_steps` 250M (150M); `mappo.gamma` 0.998 (0.997); `chunk_length` 128 (32); `epochs` 2 (4); `look_entropy_coef` 0.004 (0.001); `vision_chunk_rows` 2048 (0); lr restated; `eval.every_env_steps` 10M (5M); `episodes` 78 (512); `heldout {sweep: 195}`; `heldout_every` 1000; `heldout_on_best` false; `report` 38 names (37); `status.headline` 17 (15); `status.targets`: 9 inherited targets dropped by `null` (arrived, arrive_seconds, time_ratio, arrive_seconds_sight, time_ratio_sight, stop_distance, overshoot, stops_near, course_kinks), set: found, found_deepest, find_seconds, sight_seconds, sight_to_arrival, rooms_before_found, revisit_rate, wall_seconds, timed_out, died; `convergence.measure` found (arrived); `fade.rungs` [1.0, 0.5, 0.25, 0.0] ([0.0]); `fade.gate_metric` found, `gate_value` 0.8 restated, `require_plateau` false (default true); `costs.gate_metric` found; `layout_sampling.metric` found |
| `move3_interact` (move2_seek) | `total_env_steps` 200M (250M); `eval.episodes` 64 (78); `heldout {sweep: 60}` (195); `report` 40 names; `status.headline` 16; `status.targets`: found, found_deepest, find_seconds, rooms_before_found, revisit_rate nulled; `convergence.measure` right_object; `fade.rungs` [1.0, 0.5, 0.0] (4 rungs), gate right_object 0.8; `costs` disabled (restated), gate right_object; `layout_sampling.metric` right_object |
| `move4_follow` (move2_seek) | `mappo.gamma` 0.999 (0.998); `eval.episodes` 64 (78); `heldout: null` (drops `sweep`); `report` 38 names (27 swapped); `status.headline` 12 (17); `status.targets`: 8 of M2's nulled, follow_kept_share etc. added; `convergence.measure` follow_kept_share; `fade.gate_metric` follow_kept_share, `gate_value` 0.75 (0.8); `costs.gate_metric: null`, `gate_value: null` (both dropped: cost ladder off anyway); `layout_sampling.metric` follow_kept_share |
| `combat1_fight` (root) | `total_env_steps` 300M; `seed_from` latest; same runtime block as move1 (overlap, torch_threads 4, checkpoint_every 100); `mappo`: gamma 0.998, chunk_length 128, epochs 2, actor/critic lr 1.5e-4, vision_chunk_rows auto, look_entropy_coef 0.004 (M2's values, set here since the A6 fix; they were 3e-4 and 0.001); `eval`: every 10M, episodes 240, sampled_every 3, trace_episodes 32, 30-name report; `status`: 11-name headline, 7 targets; `convergence.measure` won, patience 3; `fade`: enabled, rungs [1.0,0.5,0.25,0.0], gate won 0.7, require_plateau false, moving_classes 1000; `costs` disabled, gate won 0.7; layout_sampling metric won |
| `combat2_packs` (combat1_fight) | `mappo.gamma` 0.999 (0.998); `eval.report` 34 names (30); `status.headline` 12 (11); `status.targets` kills, kill_seconds, selected_share, ally_deaths nulled and 7 changed; `fade` and `costs` restated (no effect); `total_env_steps` restated |
| `combat3_survive` (combat2_packs) | `eval.report` 35 names; `status.headline` 12; `status.targets` interrupts, interrupt_earnings, fire_share, target_in_view nulled; `convergence.measure` survived (won); `fade.gate_metric` survived (won); `costs.gate_metric` survived; `layout_sampling.metric` survived |
| `group1_roles` (combat3_survive) | `total_env_steps` 400M (300M); `eval.episodes` 384 (240); `report` 52 names; `arms {with_human: 64, with_partners: 64}`, `arms_every` 2; `status.headline` 15; `status.targets` survived, packs_cleared, hurt_share nulled, 13 set; `status.excluded {death_knight: ...}`; `convergence.measure` won; `fade.gate_metric` won, `gate_value` 0.6 (0.7); `costs.gate_metric` won, 0.6; `layout_sampling.metric` won; `cast.partners`: stages [combat3_survive], snapshot_every_env_steps 20M, newest_share 0.3, share 0.3, max_partners 2, pool_size 6, keep_newest 2, score won, eval_partners [combat3_survive] |
| `dungeon2_ragefire` (group1_roles) | `total_env_steps` 1.5B (400M); `mappo.goal_entropy_final_fraction` 0.5 (0.05); `gamma` 0.999 restated; `eval.every_env_steps` 20M (10M); `episodes` 64 (384); `seed` 1000 and `deterministic` true restated; `sampled_every` 4 (3); `heldout {heldout: 16}`; `report` 45 names (52); `arms {with_human: 32, with_partners: 32}`; `status.headline` 13 (15); `status.targets`: the 13 group1 targets nulled, 5 set; `status.excluded` replaced text; `convergence.measure` full_clear (won); `fade.gate_metric` full_clear, `gate_value` 0.5 (0.6); `costs.gate_metric` full_clear, 0.5; `layout_sampling.metric` full_clear; `cast.partners.stages` and `eval_partners` [group1_roles] (was [combat3_survive]) |
| `dungeon3_deadmines` (dungeon2_ragefire) | `total_env_steps` 2B (1.5B); `eval.report` 50 names (45); `status.headline` 18 (13); `status.targets` boss_oggleflint, boss_taragaman, boss_jergosh, boss_bazzalan, wing_rejoin_seconds nulled, 5 set; `convergence.measure` bar_clear (full_clear); `fade.gate_metric` bar_clear (gate_value 0.5 restated; `fade.enabled` inherited true); `costs.gate_metric` bar_clear; `layout_sampling.metric` bar_clear; `cast.partners.stages` and `eval_partners` [dungeon2_ragefire] |

No automated check reads these yamls any more. By hand: load each with `TrainConfig.load` (a typo is an error), compare
the `gate_metric`/`measure`/`headline` names with `apps/forge/tools/sim_metrics.py --stage <stage>`, and run
`apps/forge/tools/resume_check.py --fresh --all` for the learner side.

`fast.yaml` (overlay): `seed_from latest`, `total_env_steps` 10M (fallback; the sim passes the budget), `rollout_length`
128, `checkpoint_every` 10, `keep_checkpoints` 2, `mappo.minibatches` 4, `eval.every_env_steps` 1M, `eval.episodes` 64,
`convergence.patience` 4 and `window` 3 (the sim forces `patience=0`).

## Observed issues

- Stale commentary in the yamls: the `PartnerConfig.stages` comment in `config.py` still cites `stage_c3_survive`;
  `StatusConfig` and `ConvergenceConfig`
  comments cite `stage9_deadmines`, `stage3_rotation`, `stage4_duel` (archived); `FadeConfig` cites "stage9's 1e12".
- `combat1_fight.yaml` is a second full root (173 lines) that repeats most of `move1_controls.yaml`'s `mappo` block with
  small differences (chunk_length 128 vs 32, epochs 2 vs 4, vision_chunk_rows auto vs 0; lr and look entropy now equal M2's). A shared
  base
  file would remove the duplication but would change which values a stage inherits.
- Many "restated" keys (e.g. combat2/3's whole `fade` and `costs` blocks, dungeon2's `eval.seed`/`deterministic`) change
  nothing; they make a diff of two yamls read like a change.
- `distill` and `cast.agents` are set by no live yaml (no `distill:` key in the 11 files); `style:` appears only with
  `enabled: false` (`move1_controls.yaml:208`, `combat1_fight.yaml:172`).
- `PartnerConfig.deterministic` and `CastConfig.deterministic` have no reader outside `cast.py`/`partners.py` found by
  the
  search (`cast.py:45, 197`); confirm they are used.
- `TrainConfig` mixes per-run settings with values the sim injects (`rank`, `ranks`, `local_rank`, `local_ranks`,
  `dist_*`, `cluster_sims`, `cluster_timeout`): a yaml can set them, and the sim's `--set` overrides.
- `config.py` imports `animus.human.motion` (for `MOTION_WINDOW`) and `animus.mappo.trainer`, so loading a yaml imports
  torch (the host python without torch cannot load a config).
- `config.py:592-595` places the method `local()` between two groups of fields in the dataclass body.
- `eval.score`'s value is validated only when `score_column()` is called, not at load.
- `merge` treats an explicit empty map as "replace with empty" (`config.py:784-787`: only a non-empty map merges). So
  `heldout: {}` clears held-out arenas, but `status: {}` or `fade: {}` resets that whole section to its dataclass
  defaults rather than meaning "no change".

## Reviewer notes

- Moving the sim-injected fields out of `TrainConfig` would let a yaml be validated as a stage definition alone.
- The `extends` chain makes an old stage's change propagate to every later one (e.g. a `group1_roles` value reaches
  both dungeons). Any edit should be checked by loading every yaml and with `resume_check.py --fresh --all`.
- `null` falling back to the dataclass default, not the parent's value, is easy to misread; `move4_follow`'s
  `costs.gate_metric: null` relies on it.
