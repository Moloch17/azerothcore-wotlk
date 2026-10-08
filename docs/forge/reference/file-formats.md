# File formats

Purpose and scope: every file the forge writes or reads, who writes it, who reads it, and what is in it. Written from the
code at `forge` bd32b9dc8, checked against two real directories read-only:
`var/backups/2026-10-07/move2_seek/` (called "the backup") and `var/animus-forge/shared/runs/move2_seek/` (called "the live
run"). **Both were produced by a build older than the current source** (their stage.json lists 235 episode columns and 103
reward terms; the current source defines about 225 columns and 52 reward terms; their summaries still carry `livelocked`,
`clean_kill`, `phases`). Where a real file and the code disagree, the code is described and the difference is noted.
The wire is in [protocol.md](protocol.md); episode-info and summary columns in [metrics.md](metrics.md); configuration keys
in [config-keys.md](config-keys.md) and [config-yaml.md](config-yaml.md); stages in [stages.md](stages.md).

## Map of the code that reads and writes these files

| Path | Lines | Role |
|---|---|---|
| apps/forge/python/animus/train.py | 2309 | `RunLogger` (metrics.csv, layouts.csv, tb), `EvalLog` (eval.*, stage.jsonl), checkpoints, spec.json, config.yaml, stage.json copy, finished.json, eval_baseline*.json, eval_motion.npz call. |
| apps/forge/python/animus/progress.py | 165 | progress.json writer (`write_progress`, `ProgressWriter`). |
| apps/forge/python/animus/runs.py | 100 | archive directory, checkpoint rotation, `best_rung<k>.pt`, `finished.json` name, resume check. |
| apps/forge/python/animus/stages.py | 115 | stage.json loader and layout-signature helpers. |
| apps/forge/python/animus/export.py | 900 | The `.amdl` writer and a reference reader. |
| apps/forge/python/animus/partners.py | 456 | partners.json, `partners/*.pt`. |
| apps/forge/python/animus/human/realism.py | 136 | eval_motion.npz writer; human_reference.json reader. |
| apps/forge/python/animus/human/motion.py | 201 | The motion features that npz holds. |
| src/server/game/Animus/Scenario/Curriculum/StageScenario.cpp | (large) | `WriteStageFiles` (stage.json and the layout manifests), `AppendRunEvent` (events.log). |
| src/server/game/Animus/Scenario/Curriculum/Layout/Layout.cpp | | `Layout::Manifest` (`<model>.json`, manifest format 9). |
| src/server/game/Animus/Scenario/Curriculum/Blocks/LayeredField.cpp | | `.field` files. |
| src/server/game/Animus/Vision/EvalVideo.cpp | | Evaluation videos, sidecars, index. |
| src/server/game/Animus/AnimusForge.cpp | | Camera audit (`audit.csv`, PNGs), bench.json, eval video naming. |
| src/server/game/Animus/Console/Progress.cpp | | Reads progress.json and finished.json for `forge status`. |
| apps/forge/tools/stage_json_diff.py, resume_check.py, run_snapshot.py | | Read stage.json / spec.json / checkpoints offline. |

## Run directory layout

`AnimusForge.OutputDir` (resolved in ForgeConfig.cpp:268; in the cluster deployment `var/animus-forge/shared`) contains:

```
<OutputDir>/
  layouts/<stage>/stage.json            the sim writes (WriteStageFiles) when it builds the stage
  layouts/<stage>/<model>.json          one layout manifest per class (format 9)
  runs/<stage>/                         the learner's run directory (below)
  archive/<stage>-<YYYYmmdd-HHMMSS>[-n]/  earlier runs moved aside by a fresh start (runs.archive_run)
  probes/*.field                        (AnimusForge.Probe.Dir default, ForgeConfig.cpp:274: <learner work dir parent>/probes; here var/animus-forge/probes or shared/probes, UNVERIFIED which on a given machine)
  bench/ bench.json, bench/runs, bench/layouts   the auto-tune benchmark (AnimusForge.Bench.*)
  fast/  runs, layouts                  the `forge fast` profile (AnimusForge.Fast.OutputDir)
```

`runs/<stage>/` while a stage trains (live run listing): `config.yaml`, `spec.json`, `stage.json`, `metrics.csv`, `layouts.csv`,
`progress.json`, `eval.csv`, `eval.jsonl`, `eval_episodes.jsonl`, `eval_trace.jsonl`, `stage.jsonl`, `eval_motion.npz`,
`eval_baseline.json` (only with `eval.baseline`), `latest.pt`, `best.pt`, `best_rung<k>.pt`, `checkpoint_NNNNNN.pt`, `finished.json`
(when the stage was decided), `partners.json` and `partners/*.pt` (with partners), `events.log` (written by the sim, dungeon
ladder alarms), `RUNLOG.md` (by hand), `camera/` (audit images, written by the sim), `videos/<label>/` (eval videos, sim), `tb/`
(TensorBoard events), and `metrics-before-<stamp>.csv` / `layouts-before-<stamp>.csv` (rotated files, below). `archive/` is a
sibling of `runs/`, not inside it, so TensorBoard on `runs/` does not load old runs (runs.py:3). `runs/_finetune/<stage>` and
`runs/teacher_ragefire` exist in the live tree and are used by configs as named checkpoints (UNVERIFIED who creates them; not
written by the current code).

Written atomically (write to `*.partial`, then rename): checkpoints, `best_rung<k>.pt`, progress.json, `.field` files, `.amdl`,
`eval_motion.npz` (`.partial.npz`). Appended: metrics.csv, layouts.csv, eval.*, events.log, stage.jsonl. Not atomic: spec.json,
config.yaml, stage.json copy, finished.json, partners.json, eval_baseline.json.

**Rotation of CSVs** (`train.py:_rotate`): on a resume, if metrics.csv / layouts.csv / eval.csv exists and its header is not exactly
the columns the new run would write, the old file is renamed `<stem>-before-<YYYYmmdd-HHMMSS><suffix>` and a new file starts
(printed). So a column added by a rebuild splits the history. In the live run `metrics-before-20261007-110448.csv` is one.

## stage.json

Writer: `StageScenario::WriteStageFiles` (StageScenario.cpp ~1237-1510), `format` 3 (`STAGE_FILE_FORMAT`, line 133), written with
`WriteIfChanged` to `<layouts>/<stage>/stage.json` each time the sim builds the stage, before it accepts a learner. The learner copies it into
`runs/<stage>/stage.json` (train.py:606), puts it in every checkpoint (`"stage"`), and seeds, exports and resumes against it.
Readers: `stages.py`, `bootstrap.py`, `evaluation` (categories), `export.py`, `partners.py`, `cast.py`, `tools/stage_json_diff.py`,
`tools/resume_check.py`, deploy gate. Size: 148 KB (the tuning dump dominates).

Top-level keys written by the current code:

| Key | Type | Meaning |
|---|---|---|
| format | int | 3. |
| stage | string | Stage name (`StageDefinition::Name`). |
| suffix | string | Model name suffix, e.g. `_seek` (model = class + suffix). |
| extends | string | The stage seeded from ("" for the first). |
| summary | string | One-sentence description. |
| seats | int | Seat count of the stage (agents beyond seats are cast owners). |
| blocks | [string] | Block names in layout order (core, move, ...). |
| arenas | [object] | Per arena, see below. The episode info column `arena` indexes this list. |
| cast | [object] | `{agent, name}` for a cast owner row ("leader" or "owner"); `[]` for all live stages. |
| seed_chain | [string] | Stages seeded from, closest first (walk of `Extends`). |
| merges | [string] | Further parents of a merge stage (group1_roles merges move4_follow). |
| state | {arena_first, arena_count} | Where the critic state holds the arena one-hot (`STATE_ARENA_FIRST`, `MAX_ARENAS`=16). |
| models | {class: model} | e.g. `warrior: warrior_seek`. |
| layouts | {class: object} | Per class, see below. |
| episode_info | [string] | Episode-info column names in wire order (the SPEC's list). |
| episode_categories | {column: [names]} | Columns that index a name list: `drill_pack`, `seek_room`, `seek_object`, `interact_site`, `interact_object`, `sight_object`, `objective_corner` (["in_sight","corner"]). |
| reward_terms | {term: "outcome"\|"cost"\|"shaping"} | Category of every reward term (`RewardTermCategory`). |
| goals | object | `kinds` [12 names], `accepts` [kind][target] 0/1, `targets` (29 in the backup), `block` "goal", `columns` {secondary_ended, event, from_order, order_kind, order_target, achieved_kind, achieved_target, width} (first-column offsets inside the goal block), `slots_on_wire` (2). |
| tuning | object | `CurriculumTuning::Json()`: every `AnimusForge.Curriculum.*` value in force (725 keys in the backup, e.g. `Characters.HighLevelFirst: 61`), written or default. This is what the cluster fingerprint hashes (protocol.md section 9). |

`arenas[i]` (current writer): `name`, `weight` (the arena's draw weight as configured), `seats`, `episode_seconds`, `plan`
("solo" or "party"; note: the code only ever writes these two, although `partners.py` and a comment mention "raid"), `eval_only`,
`pull_drill`, `corridor_packs`, `stand_in_share` (percent), `drill_seat` (0 when `DrillRole`, else -1). The backup's arenas also have
`pvp`, `ambushers`, `checkpoints`, `team_seats`, `lone_seats`, `directed`, `taught` and the top level has `director_agents`: keys the current
writer no longer emits (a diff of old and new stage.json will report them removed; `stage_json_diff.py --allow-removed-keys`).

`layouts.<class>` : `obs_dim`, `num_actions`, `action_names` [N], `spec_names` (the class's builds in the order the `spec` episode column
indexes), `spec_roles` ("tank"/"healer"/"damage" per build), `sets` (seat entity sets, `DescribeSeatSets`; `[]` in the backup),
and `blocks` [ {name, obs [first,count], actions [first,count], revision? , action_features? (core), rescaled? [{tag,first,count}] (core),
obs_names? (named columns, e.g. move: 57 names), plus per block manifests: vision: `image` {height 64, width 128, channels 5, bytes_per_pixel 5,
class_byte 3, slot_byte 4, classes 24 with `class_names`, `class_kinds`, `entity_slots` 32, `patch` 8, `render_sizes` [[32,16],[48,24],[64,32],[128,64]],
`render_weights`}, `camera` {mode "free, never adjust", yaw_rates, pitch_rates, ...}, `look`; entities: `entities`; map: `map` {height 48, width 48, cell 2.0,
heading_up, channels 6, channel_names, map_bytes 13824, code_names [unknown, floor, wall, door, hazard], ...}; sight: `sight` } ].
Block revision numbers in the backup: core 1, move 5, vision 5, entities 1, map 1.

Compatibility rule: `layout_changes` (stages.py:84) compares per-layout signatures (sha1 of block names, spans, revisions); a resume
whose stage.json differs in any block span or revision is refused (train.py:_load_or_seed), and `resume_mismatch` checks the spec keys.

### Layout manifests `<layouts>/<stage>/<model>.json`

`Layout::Manifest` (Layout.cpp:~190), `MANIFEST_FORMAT = 9`: `format, model, stage, class_name, class, obs_dim, num_actions, action_names,
specs` [{name, tree, aptitude{feature: value}}], `blocks` [{name, obs, actions, revision?, plus `DescribeManifest` output}]. Copied beside an
`.amdl` by export. (Not read by the learner at training time.)

## spec.json

Writer: leader learner at start (train.py:601), `json.dumps(asdict(spec), indent=2)` of the `Spec` dataclass (protocol.py): `version,
num_envs, agents_per_env, obs_dim, state_dim, num_actions, episode_info_dim, goal_count, tick_ms, decision_ticks, episode_seconds, scenario,
layouts [{name, obs_dim, num_actions}], episode_info_names [..], env_groups, kinematics_dim, image_bytes, look_heads, map_bytes`. The live
run's file is `version: 24`, 192 envs, 1 agent per env, obs_dim 1595, 235 episode info names. Readers: `bench_learner.py --spec`,
`tools/resume_check.py --spec` (the new build's shapes), and `resume_mismatch` takes the same dict from a checkpoint's `spec`.
`num_envs` is the *learner's* env count (this rank's), not the pool's.

## config.yaml

Writer: leader learner at start (train.py:582): `yaml.safe_dump(config.to_dict(), sort_keys=False)`, the full resolved `TrainConfig`
(after `extends`, overlays and overrides). 5.5 KB in the live run. Also stored inside every checkpoint under `"config"`.

## metrics.csv

Writer: `RunLogger` (train.py:105+), one row each `log_every` updates, flushed per row; `csv.DictWriter(restval="", extrasaction="ignore")`.
Columns are fixed at start (train.py:785-855): `update, env_steps, env_steps_per_sec, update_seconds, reward_per_decision, episodes`, then one
`episode_<name>` for every SPEC episode-info name, then `policy_loss, value_loss, entropy, entropy_coef, clip_frac, approx_kl, explained_variance,
actor_grad_norm, critic_grad_norm, epochs_run, allowed_actions, approx_kl_move, epochs_done, minibatches_done, lr_scale, shaping_scale, cost_scale,
ladder_collapsed, ladder_stalled, frozen_layouts, cast_rows, cast_fallback_rows, partner_rows, partner_fallback_rows, partner_members,
partner_episodes, stand_in_episodes, stand_in_unfielded, elapsed_seconds, update_compute_seconds, distill_coef, distill_kl, distill_rows`, then
optional groups: style (`style_reward, style_scale, style_disc_human, style_disc_bot, style_gp, style_disc_loss, style_reward_<context>`),
explore (`explore_cells, explore_deepest`), goal head (`goal_swap_action_change, hindsight_loss, hindsight_rows, goal_entropy,
goal_kept_share, goal_<i>_share, goal_targeted_share`), look head (`look_entropy, look_turning, look_pitching, look_zooming`, and the five
`LOOK_COMMANDS` shares), slow goal loop (`slow_policy_loss, slow_value_loss, slow_approx_kl, goal_reached_share, lookahead_loss, lookahead_brier,
lookahead_brier_base, lookahead_duration_error, goal_best_by_lookahead`) and foresight (`foresight_loss, forecast_health_8_error,
forecast_health_20_error, forecast_goal_reached_16_brier`). `episode_<name>` is the mean over the episodes that ended in this update, with the
per-event weighting of `episode_means.PER_EVENT` (metrics.md). Columns absent in an update (no episodes ended) are blank. `weight_sync_seconds`
and other `stats` keys reach `row.update(stats)` but are written only if they are in the fixed column list (extrasaction ignore).
The live run's file has 327 columns (older build: includes `hint_*`, `scripted_share`, `cast_members` that the current code no longer lists).
Rows hold training-time, sampled-policy numbers; evaluation numbers are in eval.* and progress.json. The same row (minus ignored keys) is sent to TensorBoard
scalars when `torch.utils.tensorboard` imports.

## layouts.csv

Writer: `RunLogger.log_layouts` called from `log_layout_rows` (train.py:2141) on the same cadence, **one row per (class, build)** of the update's finished training
episodes (the docstring says "(class, role)": stale). Columns come from the first row written: `update, env_steps, layout, spec, episodes, entropy,
approx_kl, allowed_actions, lr_scale, frozen`, then `episode_<name>` for all episode info names. `spec` is the build name from stage.json `spec_names`
(or the index as text). Entropy/KL/allowed are that class's convergence signals (`trainer.layout_stats`). The live run's file is 100 MB (245 columns): it grows
without rotation by size. Read by humans and `tools/run_snapshot.py` (UNVERIFIED which tools read it).

## progress.json

Writer: `ProgressWriter.write` (progress.py) after every update, around every evaluation (`phase` "evaluating"), at start (`training`) and at the end
(`finished` if the stage was decided, else `stopped`). Atomic (`.partial` then rename). Flat JSON object, indent 1, numbers/strings/null only; bools
become 0/1; NaN/inf become null and the key is named in `nonfinite` (comma list) unless the key is in `undefined` (per-event means with no event).
Reader: the sim (`ProgressFile::Parse`, Console/Progress.cpp: top-level numbers, strings, booleans; nulls dropped; nested values skipped).
The sim uses it for `forge status`, the periodic report, `forge list`, a stale-file check (`started_at`) and, notably, **to name the eval video directory**
(`phase == "evaluating"` and `env_steps`, AnimusForge.cpp:1564): a hidden learner-to-sim file contract.

Keys, by source:

| Keys | Source | Who reads them |
|---|---|---|
| `run_name, scenario, total_env_steps, started_at, resumed_update, resumed_env_steps, eval_every, patience, window, baseline, status_headline, status_targets, status_excluded` | `ProgressWriter.static` (set once) | sim: total_env_steps, started_at, patience, eval_every, window, baseline, status_* |
| any `note(key, text)` (today only `stand_in`) | `ProgressWriter.note` | sim prints `stand_in` in status |
| `phase, update, env_steps, updated_at, finish_reason, advanced` | `write()` | sim (`phase`, `update`, `env_steps`, `updated_at`); `advanced`/`finish_reason` are for finished state |
| every metrics.csv column of the last logged update (`env_steps_per_sec, reward_per_decision, entropy, value_loss, approx_kl, clip_frac, episodes, episode_<name>, shaping_scale, cost_scale, ladder_collapsed, ladder_stalled, lr_scale, ...`) | `training(row)` | sim reads `env_steps_per_sec`, `reward_per_decision`, `entropy`, `value_loss`, `approx_kl`, `clip_frac`, `episodes`, `episode_<column>`, `ladder_collapsed`, `ladder_stalled`, `update` |
| `evals, last_eval_env_steps, last_eval_score, baseline_score, best_score, best_env_steps, evals_since_best, converged_layouts, active_layouts, weakest_layout, weakest_missing, reentries` | `evaluated()` | sim (status table, ETA) |
| `eval_<metric>` for each `status.headline` metric | `evaluated(summary=...)` | sim (headline table beside `episode_<metric>`) |
| `eval_<arm>_score, eval_<arm>_episodes, eval_<metric>_<arm>`, and for arm `with_human`: `eval_clear_standin`, `eval_standin_gap` | `arm_evaluated()` | sim via headline names like `clear_rate_with_human` |
| `nonfinite` | `write_progress` | sim warning line |

The sim also reads `finished.json` for the stage list (`ForgeCommands.cpp:398`, `AnimusForge.cpp:985` key `advanced`, `reason`).
The live file includes keys no longer in the code (`look_*` shares are still there; `hint_*` were removed). `ladder_stalled` is in the current code (train.py:2049) but not in the live file.

## finished.json

Writer: `TrainingRun.finish` (train.py:2247), only when the stage was decided (`outcome`): `{reason: "converged"|"budget", advanced: bool, env_steps, update,
best_score, best_env_steps, layouts: {class: {converged, reentries, missing[], score, kl, entropy, rung, top_rung}}}`. Deleted on a resume (train.py:579).
Reader: the sim, to advance the plan (`advanced`) and show the state.

## stage.jsonl

Writer: `EvalLog.write_outcome` after each controller decision (train.py `handle`): one line `{update, env_steps, action, reason, layouts: <the controller report as in finished.json>}`.
`action` is "continue" or "advance" (stage.py:49). Not present in the live run listing (older build or nothing decided).

## eval.csv

`EvalLog.write` (train.py:~165). Header: `update, env_steps, policy, episodes, score, stderr, margin, best, evals_since_best, seconds`, then extra columns only when
`style.reference` is set (`realism_emd`, `realism_emd_<context>`, `realism_disc`). One row per scored policy: `learner` (the argmax evaluation), `learner_sampled`,
`heldout_<arena>`, `with_human`, `with_partners`, and the baseline name (`random`). `margin/best/evals_since_best` are the *overall tracker's* state at that moment
(for non-learner rows they repeat the last). `score` = mean of `score_outcome` (or the return with `eval.score: return`); `stderr` is the standard error over **episodes** (agents of one episode averaged first,
`standard_error`). `seconds` is wall time of the evaluation. See the live file: 16 rows with `learner_sampled` every 3rd evaluation.

## eval.jsonl

One JSON object per eval.csv row: the row's non-empty fields (`update, env_steps, policy, episodes, score, stderr, margin, best, evals_since_best, seconds`) plus `"summary"`: the
`EvalResult.summary(columns)` dict. Summary keys (evaluation.py:~190-345): `policy, episodes, score, stderr, return`, then the mean of every column in `eval.report` +
`status.headline` that the stage reports (per-event columns weighted by their count column; `null` when no event), plus `arrived_at_rung` if the M1 compass columns exist; and groups:
`bands` (level bands 1-20, 21-40, 41-60, 61-80 by episode column `level`), `layouts` (per class, only if several), `specs` (per build name across classes), `castings` (per
`<class>_<build>`), `arenas` (by `arena` column if the stage has more than one), `builds` (talent plan: standard/noisy/random), `difficulties` (by `difficulty` tier; and
`up_to` with every tier below the top, per layout and per casting), `top_rung` (rows with `at_top_rung > 0.5`, else the highest difficulty tier; with per-layout means),
`categories` (`<column>=<name>` for each `episode_categories` column), `found_deepest` (found rate where `deep_room`), and for sampled rows `argmax_gap`. Each group entry
has the same shape: `episodes, score, stderr, return` plus the report columns. How these are built is explained in metrics.md ("Evaluation tables"). The live run's
file has 3.8 MB for 16 evaluations (about 240 KB per line). The live summaries also hold `phases`, `livelocked`, `clean_kill`, which the current code does not write.

## eval_episodes.jsonl

One line per scored agent-episode of every evaluation row written through `EvalLog.write` (including baselines, sampled, held-out and arm rows): `{update, env_steps, policy, seed,
layout, return, <every episode-info column, rounded to 4 places>, actions {action name: count, no-op excluded}, allowed {action name: decisions allowed}}`
(`actions`/`allowed` absent for a baseline). 245 keys in the live file; 9 MB for 16 evaluations. A party episode gives one line per present seat (stand-in and excluded partner rows are not scored).

## eval_trace.jsonl

When `eval.trace_episodes > 0`: for the seeded episodes with seed index < trace_episodes of each *learner* evaluation: `{update, env_steps, policy, seed, decision, agent, layout, action
(name), goal (primary goal id or -1)}` per decision per agent. One row is ~100 bytes; 25 MB in the live run (64 traced episodes x 16 evaluations).

## eval_motion.npz

`realism.write_motion`; overwritten at every learner evaluation (only the last is kept), when the sim sends kinematics (protocol 20) and `collect_motion` found tracks. numpy `savez_compressed`
with arrays `windows` f32 [N, W, 17] (W = `style.window`, default `motion.WINDOW` = 8; 17 = `motion.FEATURES`: fwd, lat, up, planar, moving, yaw_rate, course_sin, course_cos, pitch, pitch_rate, accel,
mode_ground, mode_swim, mode_fly, mode_airborne, mounted, in_combat), `context` i16 [N] (mode*4 + mounted*2 + in_combat, 16 contexts), `weight` f32 [N] (1, or inverse keep share when capped at
`style.eval_windows`), and `meta` a 0-d string holding JSON `{source:"eval", run, scenario, update, env_steps, tracks, steps, step_seconds, window}`. Same layout as the human dataset
(`human_motion_windows.npz`, FORMAT.md section 5 of the human pipeline: see [py-human-and-misc.md](py-human-and-misc.md)). `human_reference.json` (reader `load_reference`): `{"format": 1,
"motion": {context id: {name, steps, hist: {feature: counts}}}}` with histogram bins fixed by `motion.HIST_BINS` (61 edges for most features, 31 for `planar`).

## eval_baseline.json and eval_baseline_<seed>_<episodes>.json

`{"key": {policy, seed, episodes, arenas, tuning, score, shaping}, "summary": <as eval.jsonl>}`. A cache of the `random` baseline's evaluation, valid only if the key matches (tuning is stage.json's
whole `tuning`, so any tuning change replays it). Only exists when `eval.baseline: random`.

## partners.json and partners/

`PartnerPool.write` (leader): `{members: [{path, kind ("stage"/"path"/"snapshot"), episodes, party_score, retired, draw_share}], active, missing, unusable}`. `partners/<tag>.pt` are copies of checkpoints
(`partner_snapshot`): tags `step_<env_steps>` (latest.pt snapshots) and `best_<env_steps>` (improved best.pt).

## RUNLOG.md

Hand-written Markdown log of decisions per run (see the backup: dated bullets, diagnoses, config changes). **No code writes or reads it.** It travels with the run directory (the backup
contains it).

## events.log

Writer: the sim, `StageScenario::AppendRunEvent` (StageScenario.cpp:1726), path `runs/<stage>/events.log` (set only for the forge, `StageSettings::EventsLog`). Line format:
`<UTC ISO8601 %Y-%m-%dT%H:%M:%SZ> <stage>: <text>`. Only the dungeon ladder's alarm and its "cleared" notice are written (WingLadder `Result.Alarm` / `Result.Cleared`; collapse after 3 reads below
the floor, 5 at rung 0). Read by people; `forge status` only refers to it. The host's ladder decides, so on a cluster it lives on the host.

## Checkpoints: latest.pt, best.pt, best_rung<k>.pt, checkpoint_NNNNNN.pt, partners/*.pt

`save_checkpoint` (train.py:~150) = `torch.save` of a dict, to `<name>.partial` then renamed. **Keys** (verified from code; I did not open a `.pt`):

| Key | Content |
|---|---|
| `trainer` | `MappoTrainer.state_dict()` (actor, critic, value_norm, optimisers, schedules; py-mappo.md). `export.py` reads `checkpoint["trainer"]["actor"]`. |
| `config` | `TrainConfig.to_dict()`. |
| `spec` | `asdict(spec)` (as spec.json). |
| `update`, `env_steps` | counters. |
| `convergence` | `ConvergenceTracker.state_dict()`: `best, best_stderr, best_env_steps, evals_since_best, history [(env_steps, score, stderr)]`. |
| `controller` | `ConvergenceController.state_dict()`: `best_summary, baseline_summary, plateau_env_steps, baselined, evals, fade{...}, costs{...}, layouts{class: {tracker, kl, entropy, rung, top, ladder, top_scored, scores, converged, converged_score, converged_margin, reentries, played}}`. |
| `stage` | the stage.json dict at save time (for seeding later stages). A checkpoint older than this key is read with the run directory's stage.json (`load_parent`, cast.py). |
| `score_kind` | "" (return) or "score_outcome". |
| `explore` | Go-Explore archive (if enabled). |
| `style` | style discriminator state (if enabled). |

Files: `latest.pt` is rewritten at every checkpoint (`checkpoint_every` updates or `checkpoint_env_steps`) and at the end; `checkpoint_<update:06d>.pt` is written alongside, and only the newest
`keep_checkpoints` are kept (`prune_checkpoints`); `best.pt` is written when an evaluation improves on the best by the margin (the best of the **current rung** on a gate-stepped ladder: the next rung's first
evaluation overwrites it); `best_rung<k>.pt` (fade ladder; `best_<ladder>_rung<k>.pt` for another) is a copy of best.pt made when the ladder leaves rung k, outside the rotation glob. Size ~123 MB each in the live run
(about 8 files per directory). `weights_only=False` is used to load them (pickled Python objects: loading a checkpoint executes code, so only load your own).

## The `.amdl` model format

Writer `export.write_amdl` (export.py:~185), `AMDL_VERSION = 9`, readable versions (7, 8, 9); the format is documented field by field in the export.py module docstring (lines 6-83) and I verified the writer matches it in order.
Little-endian: `"AMDL"`, `u32 version`, `u16 name length` + UTF-8 name, `u32 obs_dim, num_agents, num_actions, layer_count`, per layer `u32 in, u32 out, f32 W[out*in], f32 b[out]`, then optional sections each
prefixed by a size/flag: GRU (`u32 recurrent_size` + 4 arrays), foresight feedback, slow GRU, goal head (kinds/targets/every, weights, `accepts`, `goal_block_at`, embeddings, lookahead flag, `goal_slots`
(v6), kind/target scales (v7)), a never-written director byte (0), seat sets (v8) with the attention layer (v9), pointers. Observation normalisation is folded into the first layer; `num_agents` is 1 with a zero column.
`export_layouts` writes `<model>.amdl` per class plus a copy of the layout manifest `<model>.json`. **It refuses any stage with a vision block or a checkpoint with `vision.`/`look_head.` weights** (export.py:~443-452), and every live
stage has a vision block, so no live checkpoint can be exported today. The only reader in this repository is `export.read_amdl`/`reference_decide` (test reference); the C++ loader lives in a patch for the other module
(`apps/forge/patches/mod-animus-amdl8.patch`, `amdl8-check/`), not in the core. Tests: test_export.py, test_export_seat_sets.py, test_goal_queue.py, test_goal_targets.py.

## Camera audit (runs/<stage>/camera/)

Writer: the sim, `Forge::MaybeAuditCamera` (AnimusForge.cpp:1399), every `AnimusForge.Vision.AuditInterval` seconds while training. Per sampled seat (one per layout first): `<YYYYmmdd-HHMMSS>-e<env>a<agent>-<layout>.png` (the panels:
camera, layers, and the map crop as a fifth panel when a map block exists; scale `256 / width`) and `...-composite.png` (all layers in one picture), plus a row appended to `audit.csv`:
`time, scenario_seconds, decision, env, agent, layout, class, race, level, map, instance, x, y, z, orientation, episode_seconds, render, <one count column per pixel class: sky, terrain, model, door, water, deadly, hostile_creature,
neutral_creature, friendly_creature, hostile_player, friendly_player, quest_giver, vendor, trainer, lootable_corpse, corpse, chest, herb, ore, mailbox, quest_object, usable_object, other_object>, entities, file`. The live directory has 577 files
and no pruning; nothing deletes them (observed issue).

## Evaluation videos (runs/<stage>/videos/<label>/)

Writer: `EvalVideoRecorder` (Vision/EvalVideo.cpp), enabled by `AnimusForge.Vision.EvalVideos` > 0; only the learner's play (not a baseline) is filmed. `<label>` is the learner's `env_steps` read from progress.json while its phase
is "evaluating", else `d<decision ticks>` (a worker); a held-out arena adds `-heldout<arena index + 1>` (so `videos/131158016-heldout1/`). Files per filmed episode:

* `<seed>-<layout word>-<outcome>.png`: an **animated PNG** (APNG), frame size `camera.Width*Scale x camera.Height*Scale`, one frame per decision, frame delay `DecisionMs`, composite with the map inset where a map exists. `<outcome>` is
  `success`/`failure`/`ended`/`unfinished` from the first of the columns `found, arrived, won, cleared, success, survived` the stage reports (> 0.5 is success).
* `<same stem>.json` sidecar: `file, scenario, evaluation, seed, env, agent, layout, class, race, level, role, rung (the first column named `rung`/`tier`/`*_rung`/`*_tier`, null if none), outcome, frames, dropped_frames, bad_frames,
  truncated, decision_ms, fps, width, height, scale, camera ("WxH"), render ("WxH" the camera cast at, or null), map_inset, bytes, encode_ms_per_frame, measures {every nonzero episode-info column}`.
* `index.json` (`scenario, evaluation, selected_seeds, videos [sidecars]`) and `index.html` (a page showing them all), written when the evaluation ends and every video has been written.

Seeds filmed: `EvalVideoSeeds(first, end, count, pairs)` spreads `count` picks over (class, build) pairs and rungs, identical every evaluation. Frames queued past 512 MiB are dropped (counted in `dropped_frames`). `forgectl videos` and
`apps/forge/tools/collect-videos.sh` gather them from workers.

## Probe data: `.field`

`LayeredField::Write/Read` (LayeredField.cpp). File name `<AnimusForge.Probe.Dir>/<mapId:03>_<gridX>_<gridY>.field`. Layout: a header struct (natural alignment, no pragma; by member sizes it is 48 bytes with no padding, UNVERIFIED by sizeof)
`u32 Magic = 0x464C4841 ("AHLF" as a little-endian u32), u32 Version = 1, u32 MapId, i32 GridX, i32 GridY, f32 Cell, u32 Side, f32 MinX, f32 MinY, u32 Intervals, u64 RawBytes`, then one **zstd level 15** stream of `RawBytes` bytes:
`Side*Side` bytes (interval count per cell, capped at 255) followed by `Intervals` x 8-byte `Interval { i16 Floor8 (1/8 yd), u16 Headroom8 (0xFFFF = sky), i16 Liquid8 (-32768 none), u8 LiquidFlags, u8 Flags (low 4 bits nav flags, 0x80 OpenAbove) }`.
Reader validates magic, version, `Side` in 1..8192, `RawBytes <= 2^32` and the total. Cells are 1 yd (`STANDARD_CELL`); about 0.3 MB a grid; baked by the `forge fieldstage` / `forge fieldworld` console commands. The cluster fingerprint counts `*.field` files and
their bytes only. There is **no `.probe` file** in the code: the briefing's ".field/.probe" has no second format (grep for `.probe` finds none).

## Other files

* `bench.json` (`<OutputDir>/bench/bench.json`): the auto-tune result written by the sim (`AnimusForge.cpp:2137`), `{... "best": <trial>}`; read back at startup to avoid a re-bench (`HasBenchForThisCpu`, ~1732). Fields: UNVERIFIED (see cpp-runtime.md).
* `env/dist/logs/animus-learner.log` (`LearnerLogFile`): the learner's stdout, tailed by `forgectl`.
* `tb/events.out.tfevents.*`: TensorBoard scalars of metrics.csv rows and evaluations (`eval/<name>`, `eval_<group>/<name>`).
* The conf files: `env/dist/etc/modules/mod_animus_forge.conf` per machine (cluster.md). **No `mod_animus_forge.conf.dist` is tracked in this tree** (`git ls-files` finds only authserver/worldserver/dbimport dist files), although
  ForgeConfig.h:37 says "mod_animus_forge.conf.dist documents every key". `AnimusForge.Curriculum.*` keys must match across machines (239 on 2026-10-07 per cluster.md); `forgectl conf-sync` copies them. Keys: [config-keys.md](config-keys.md).
* `apps/forge/cluster.toml`: forgectl's machine list ([tools-and-ops.md](tools-and-ops.md)).
* Learner yaml configs `apps/forge/python/configs/<stage>.yaml` (and `fast.yaml`): [config-yaml.md](config-yaml.md).

## Observed issues

* The real backup and live run come from a build older than the source; any tool that diffs them against current output will show removed keys (arena `pvp` ... , `director_agents`, 100+ reward terms and ~10 episode columns).
* layouts.csv is 100 MB for one stage and has no size cap or rotation other than header change.
* `camera/` audit images are never pruned (577 files in the live run).
* `RunLogger.log_layouts` docstring says (class, role); rows are (class, build).
* progress.py's docstring points at `src/Console/Progress.cpp` (now `src/server/game/Animus/Console/Progress.cpp`).
* `evaluation.standard_error` docstring refers to `animus.gates.noise_allowance`, a module that does not exist.
* train.py:`_checkpoint_extra` stores the whole stage.json in every checkpoint (148 KB x every file) so a checkpoint can seed stages without the stage file; `load_parent` still has a fallback for older ones.
* Two writers of the same information: the sim copies `stage.json` into `layouts/` and the learner copies it into `runs/`; they can differ if the sim rebuilt the stage after the learner started (the learner's copy is what the checkpoints carry).
* The video directory label depends on reading the learner's progress.json (hidden coupling, falls back to `d<ticks>` on a worker).
* `partners.py` and comments mention a "raid" plan; stage.json only ever says "solo" or "party".
* No `.pt` was opened in this review; the key list is from the writer.

## Reviewer notes

* Anything that changes a column list (episode info, metrics) splits metrics.csv/layouts.csv on resume and changes the fingerprint-visible `episode_info` in stage.json; `stage_json_diff.py` is the guard.
* `weights_only=False` everywhere: consider a safe loader before checkpoints cross trust boundaries (the cluster fetches them via `async_sync`).
* Decide whether `.amdl` export is still wanted: nothing in the live curriculum can use it.
