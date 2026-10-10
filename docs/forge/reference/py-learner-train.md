# Python learner: `TrainingRun`, the loop, and the wire

Part of [py-learner.md](py-learner.md). Covers `train.py` (2309 lines), `env.py` (515), `protocol.py` (558),
`device.py` (111), `blas.py` (78), `evaluation.py` run side (750), `episode_means.py` (108), `rewards.py` (88).
Line numbers: commit `bd32b9dc8`. The convergence rule is in [py-learner-stage.md](py-learner-stage.md); networks,
`MappoTrainer`, ranks and the async hub are in [py-mappo.md](py-mappo.md) and
[py-human-and-misc.md](py-human-and-misc.md); the wire format in [protocol.md](protocol.md); the files written in
[file-formats.md](file-formats.md) and [metrics.md](metrics.md).

## Entry point and argv

`python -m animus.train` runs `main()` (`train.py:2271`): parse args, `TrainConfig.load(config, --set...,
--overlay...)`,
apply `--socket`, `--run-name`, `--runs-dir`, `--layouts-dir` over the config, `blas.prepare(train device)`, then
`TrainingRun(config, resume=--resume).run()`. The return value 0 is the exit code. The exit code is 0 whether the stage
converged or hit its budget (`train.py:17`); an exception (including `ConnectionError` when the sim goes away) leaves
the process with a traceback and a non-zero code, after `finish()` has run in the `finally` (`:2235-2238`).

Options: `--config` (required), `--socket`, `--run-name`, `--runs-dir`, `--layouts-dir`, `--overlay YAML` (repeatable,
merged over the config before `--set`), `--set KEY=VALUE` (repeatable, YAML-parsed, dotted keys), `--resume`.

### What the sim passes (`src/server/game/Animus/Learner/LearnerProcess.cpp:80-155`)

Argv, in order: `<AnimusForge.Learner.Python> -u -m animus.train --config <LearnerConfigFor(scenario)> --socket <path>
--run-name <scenario> --runs-dir <RunsDir> --layouts-dir <LayoutsDir> [--resume]`, then `--set` pairs:

| `--set` | When |
|---|---|
| `torch_threads=N` | `AnimusForge.Learner.TorchThreads` non-zero |
| `cluster_sims=['tcp://...', ...]` | the sim is a cluster host with worker sims |
| `rank`, `ranks`, `local_rank`, `local_ranks`, `dist_address`, `dist_iface`, `mappo.rank_sync` | `DistWorld > 1` (cluster); `mappo.rank_sync` is `weights` unless `AnimusForge.Cluster` sets `async` |
| `rank`, `ranks`, `dist_address=127.0.0.1:<free port>` | several local learners, no cluster |
| `train_device=...`, `rollout_device=...` | `RankDevice` returns non-empty |
| then `AnimusForge.Learner.Args` verbatim | always last, so they win |

A worker's `tcp://0.0.0.0:<port>` socket is rewritten to `tcp://127.0.0.1:<port>` (`:87-89`). `LearnerConfigFor`
(`ForgeConfig.cpp:739-762`): `AnimusForge.Learner.Config` if set (relative to the work dir), else
`configs/<class>/<scenario>.yaml` when one class is trained and that file exists, else `configs/<scenario>.yaml`.
The bench profile (`ForgeConfig.cpp:685-706`) and fast profile (`:708-736`) prepend their own `--set` and
`--overlay configs/fast.yaml`. The run is named after the scenario, so `run_name` in the yaml is overridden.
`forge cancel` ends the learner through `ChildProcess::Stop` (`ChildProcess.cpp:121-136`): wait for exit, then SIGINT,
then SIGKILL. The learner installs no signal handler; SIGINT raises `KeyboardInterrupt` in Python, `finally` runs
`finish()`, which saves `latest.pt`. UNVERIFIED: whether the sim closes the socket first (the `ConnectionError` path
reaches the same `finally`) and how long the 10 s grace (`ChildProcess.cpp:40`) is against a long save.

## `RunLogger` and the run-directory writers

- `_rotate(path, columns)` (`:71`): if the file exists, is non-empty and its header differs from `columns`, it is
  renamed `<stem>-before-<YYYYmmdd-HHMMSS><suffix>`, a line says which columns were added or dropped, and True is
returned
  (a new file starts). Used by `RunLogger` (metrics.csv, layouts.csv) and `EvalLog` (eval.csv).
- `RunLogger(run_dir, columns, append)` (`:103`): opens `metrics.csv` append when `append` and the header matches, else
  write with a header. Uses `csv.DictWriter(restval="", extrasaction="ignore")`: a key not in `columns` is silently
  dropped, which is why `columns` is built up front in `TrainingRun.__init__` (`:806-857`). TensorBoard
  `SummaryWriter(run_dir/"tb")` when importable. `log` writes the row, flushes, and adds every key as a scalar
  (`add_scalar(key, value, step)`: a NaN or non-number would be passed through). `log_layouts` writes
  `layouts.csv` (one row per class and build per update; header from the first row; rotated like metrics.csv).
- `EvalLog` (`:189`): `eval.csv` (fixed columns `update, env_steps, policy, episodes, score, stderr, margin, best,
  evals_since_best, seconds` plus realism columns), `eval.jsonl` (row plus the full summary), `eval_episodes.jsonl`,
  `eval_trace.jsonl`, `stage.jsonl` (the stage decision). `write` also logs TensorBoard groups only for
  `policy == "learner"`. Heldout, sampled, baseline and arm rows carry the learner tracker's `margin`, `best` and
  `evals_since_best` (`:215-217`), which is misleading for those rows. `EvalResult.episodes` counts rows (one per agent
  of each seeded episode), not episodes, so the `episodes` column and "over N seeded episodes" text are agent rows.
- `save_checkpoint` (`:170`): writes `<path>.partial` with `torch.save` then `replace` (atomic; no fsync).
  Keys: `trainer` (`MappoTrainer.state_dict()`: actor, critic, value_norm, actor_opt, critic_opt, optional vision_opt and slow_opt),
  `config` (`TrainConfig.to_dict()`), `spec` (`asdict(Spec)`), `update`, `env_steps`, then `_checkpoint_extra()`
  (`:1122`): `convergence` (tracker), `controller`, `stage` (the whole stage.json), `score_kind`, and when present
  `style` and `partner_scores` (the pool's scores, restored on resume). The partner pool's membership, the cast, the layout weights and the replay seeds are not saved.
- Other files in the run dir: `config.yaml` and `spec.json` and `stage.json` (leader, at start), `progress.json`
  (every update and around evaluations), `finished.json`, `best.pt`, `latest.pt`, `checkpoint_<update:06d>.pt`,
  `best_rung<k>.pt`, `eval_baseline*.json`, `eval_motion.npz`, `eval_motion_<env_steps>[_heldout_<arena>].npz`, `partners/`, `partners.json`, `tb/`.
- `finished.json` (`:2247`): `reason`, `advanced`, `env_steps`, `update`, `best_score`, `best_env_steps`, `layouts`.

## `TrainingRun.__init__` order (`train.py:541-903`)

1. `torch` threads; async-or-synchronous ranks (`Ranks` object); `seed_everything(seed + rank)`.
2. `run_dir = runs_dir/run_name`. A worker's learner (a non-leader whose local index is 0 on its machine) is `remote`:
   it never resumes from its own directory. Resume: `resume_checkpoint_path` (needs `latest.pt`; a leader without one
   exits with the message). Fresh start: `archive_run` moves an existing directory to `<output>/archive/<run>-<time>`
   (leader or remote). `ranks.barrier()`. A resume deletes `finished.json`. The leader writes `config.yaml`.
3. Connect: `ClusterEnv([socket, *workers])` when `cluster_sims[local_rank::local_ranks]` is non-empty, else `ForgeEnv`
   with the rollout device (device buffers possible). `spec = env.spec`. `run_envs` = sum of ranks' envs;
   `rank_envs` = each rank's env count (for sharing evaluation seeds). Leader writes `spec.json`.
4. `load_stage(layouts_dir, scenario)` (stage.json written by the sim before it accepts a learner); leader copies it.
   Derived tables: `arena_names`, `heldout` (`heldout_arenas`: refuses an arena the stage lacks or trains on),
   `action_names`, `spec_names`, `episode_categories`, `casting_roles`, `eval_action_mask` (`action_mask_table`).
5. `trainer_inputs` (the camera: refuses with `SystemExit` when the sim's SPEC and stage.json disagree on
   image bytes or look heads), then `make_trainer`. Prints the device line. `per_decision` converts `mappo.gamma`
   and `gae_lambda` (per `reference_decision_ms`) to the sim's `decision_ms`; foresight discounts likewise.
6. `recurrent_size <= 0` raises (the flat update was removed). `ConvergenceController(config, layout names)`.
   `score_kind` = `eval.score_column()` if the sim reports that column, else "" (the return). `judged_kind` =
   `convergence.measure` if the sim reports that column, else "" (the score): the overall tracker's column, saved
   in checkpoints beside `evaluation_signature`; `controller.measure` is set to it.
7. `_make_style()`, then `_load_or_seed()` (below), `trainer.set_goal_space`, then either
   `camera_columns_clear()` (resume, else `SystemExit`) or `clear_blind_columns()` (fresh).
8. Broadcast `(update, env_steps)` from the leader and `broadcast_module` each network, value_norm, style disc.
   Async: leader creates `Hub`, a follower `Link`, takes the leader's counters and sets `evaluating = False`.
9. `trainer.sync_rollout()`; `RolloutBuffer` (and a spare when `overlap_updates`); `acting_state`; the SIL note;
    the `ThreadPoolExecutor(max_workers=1)` for overlapped updates.
10. Build the metrics `columns` (fixed list at `:806-816` plus conditional groups: style, goals, look,
    slow goals, foresight), `RunLogger` (leader), `EvalLog`, `ProgressWriter`, cached baseline score,
    `progress.restore_evaluation`, and column indexes for `present`, `difficulty`, `at_top_rung`. `apply_holds()`.

### `_load_or_seed` (`:941-1090`)

Resume: load `latest.pt`; `resume_mismatch(checkpoint spec, spec)` over `runs.RESUME_SPEC_KEYS` (scenario,
agents_per_env, obs_dim, state_dim, num_actions, layouts) else `SystemExit` "start it fresh"; `layout_changes(checkpoint
stage, stage)` else `SystemExit`; `trainer.load_state_dict`; style state; `update`, `env_steps`;
`restore_evaluation_state` (with the run's `judged_kind` and `evaluation_signature`; a reason it drops state is printed and written to events.log). A fresh start that finds a `finetune_from` checkpoint (the stage's own, `{runs_dir}/_finetune/{run_name}/best.pt`) also carries its value normaliser (`trainer.value_norm`), which `seed_trainer` does not copy, so the seeded critic reads returns on the scale it was trained on; another stage's seed does not (its returns are another scale). Then the parents (also computed on resume, but seeding runs only when not resuming; the
teachers are built either way): see [py-learner-seeding.md](py-learner-seeding.md) for seeding, cast, partners and
distillation. Not checked on resume: `episode_info_names`, `image_bytes`, `map_bytes`, `look_heads`, `kinematics_dim`,
`goal_count`, `decision_ms`, and every config value (config differences are checked only by
`apps/forge/tools/resume_check.py`).

## The loop

`run()` (`:2219`): `env.reset()` (first STEP); broadcast and set the stand-in mode if a partner can play it; reset the
far side (cast, partners); write progress; run an evaluation if `eval.at_start` and the tracker has no history;
`last_eval_env_steps` = last history point or `env_steps`; `train()` inside try/finally `finish()`.

`train()` (`:2187`):

- Follower with a `Link`: loop `rollout()` + `log_update()` until the link says stopped; returns None.
- Else `while env_steps < total_env_steps`: `rollout()`, `log_update()`, `maybe_checkpoint()`; then if evaluating and
  `env_steps - last_eval_env_steps >= eval.every_env_steps`: `evaluate()`, then the leader's
  `controller.after_eval(env_steps)` is broadcast; on ADVANCE the held-out arenas are played if this evaluation did not;
  `handle(decision)` returns True to stop.
- After the loop (budget): a last `evaluate(final=True)` if the last evaluation was before the last update; then
  `controller.at_budget()` and `handle`.

### One rollout and update (`rollout()`, `:1617-1776`)

1. `buffer.reset()`. If `ClusterEnv`: `rejoin()` splices a returning worker's fresh STEP in and clears memories.
2. `pipelined` = more than one env group and no cast and no partners. Groups are the env halves (half-batch) or the
   whole pool.
3. Decision loop: `_act_on_rows_of` acts on the first decision; then repeat until the buffer is full:
   `buffer.add_decision(...)`; for each group `receive` the STEP (pipelined: `env.receive_step()`; else `env.step`);
   `style.record`; `_take_outcome_of` (rewards, dones, terminated into `RolloutOutcome`; marks cluster-dropped rows
   invalid; collects finished episodes; clears memories; returns a closure `value_ended` that values the ended
   episodes' final observations); `_act_on_rows` for the next decision of that group, sending its ACT at once; then
   `value_ended()` while the sim ticks. `buffer.add_outcome`.
4. `_act_on_rows` (`:1800`): refuse non-finite observations with a RuntimeError naming env, agent, index;
   `trainer.act_and_value` (actions, log_probs, values, foresight, goals); mark the row non-sample (`present` false) for
   frozen classes (`self.frozen`), cast rows, partner rows and stand-in rows; copy obs/state/mask into the buffer on the
   rollout stream (device inputs) or directly; `send` actions, goals (`wire_goals`) and look.
5. After the last decision: synchronise the rollout stream; `add_style`; `buffer.finish(value of last STEP, gamma,
   lambda, foresight, slow goal, obs targets)`; record mean reward, allowed actions.
6. Controller outputs for this update (leader decides, broadcast): `entropy_coef`, `lr_scale`, `shaping_scale`,
   `cost_scale` (followers read them from `link.control`; the leader publishes to the hub); `trainer.entropy_coef`;
   `goal_entropy_factor = goal_entropy_scale * schedule(goal_entropy_final_fraction, ...)`;
   `set_learning_rate_scale`; `distiller.coef = distill.coef_at(env_steps)`.
7. `update += 1`; `env_steps += rollout_length * run_envs * agents_per_env` (all seats, including frozen, cast and
   partner rows). `maybe_partner_snapshot()`. `env.set_stage_progress(env_steps / total, shaping, cost)` (PROGRESS).
8. Update: serial `trainer.update(buffer, distiller)`; `blas.save()`; `at_safe_point()`. With `overlap_updates`: join
   the
   previous update (`finish_update`: result, `blas.save`, safe point, `sync_rollout`), submit this one with
   `sync=False`, swap buffers; the stats returned belong to the previous rollout (the first returns `{}`), so the
   rollout acts on weights one update stale.

`log_update` (`:1995`): gather ended episodes from all ranks; `controller.observe_update(layout stats, lr_scale)`;
`observe_training_episodes` (difficulty, at_top_rung per class); return unless `update % log_every ==
0`;
build the row (counters, scales, ladder alarms as rung or -1, `frozen_layouts`, cast/partner stats, distill coef,
style), add `episode_<name>` means via `episode_means.means` (per-event columns weighted by their event count,
NaN when none) and the update `stats`; `observe_entropy`; `audit_reward`; `audit_progress`; `logger.log`;
`progress.training`/`write`; one console line `update N | steps S | R sps | rollout .. compute .. [sync ..] wall W sps [update-bound] | ...` (W = steps over the whole cycle, `wall_steps_per_sec`; "update-bound" when the sim waited over 20% of the cycle).
`audit_progress` prints "learning has stalled" when the KL divided by lr scale stays under `STALL_KL = 0.0015` for
`STALL_WINDOW = 10` updates after `STALL_MIN_UPDATES = 20`, repeating no more often than `WARN_EVERY = 25` updates
(`rewards.py:146`). `audit_reward` (`rewards.py`) prints when the largest shaping term earns more than
`MAX_SHAPING_SHARE = 0.5` of the largest positive outcome term. Neither acts.

`maybe_checkpoint` (`:1136`): when `update % checkpoint_every == 0` or `env_steps` advanced by `checkpoint_env_steps`
since the last (`checkpointed_env_steps` is created lazily with `getattr`), the leader writes
`checkpoint_<update:06d>.pt` and `latest.pt` (two serialisations of the same state) then
`prune_checkpoints(keep_checkpoints)`.
Every `_save` first `drain_update()`s an overlapped update.

### One evaluation (`evaluate()`, `:1243-1327`)

1. `drain_update`; `baseline_for(eval.seed, eval.episodes)` once per run if `eval.baseline` ("random"), cached in
   `eval_baseline.json` under a key of policy, seed, episodes, arenas, tuning, score kind and shaping scale.
2. `progress.write("evaluating")`; `_evaluate_share` -> `run_evaluation` on this rank's share of seeds (argmax if
   `eval.deterministic`), with `trace_episodes` and motion collection. Switches the sim to seeded MODE and back
   (`env.set_mode(True, ...)`, `set_mode(False, stand_in=...)`); every env resets, so training episodes in progress
   are cut. Reset cast and partner memories.
3. Leader: `summary = result.summary(report)`; `improved = controller.observe(summary, env_steps)`; `score_motion`; `save_routes` (raw tracks of the evaluation with seed ids, `eval.keep_motion_files` newest evaluations kept);
   `eval_log.write`; `progress.evaluated`; console block (`format_summary`, the excluded-classes line, ladder messages,
   collapse and stall alarms). If improved, `_save(best.pt)`. For each `controller.rung_exits`:
   `archive_rung_best` copies best.pt to `best_rung<k>.pt`. Decide `sampled` (every `eval.sampled_every` evaluations),
   `heldout` (`heldout_due`: final, or improved and `heldout_on_best`, or every `heldout_every`-th), `arms`.
4. Broadcast the decisions; an improved best joins the partner pool; `evaluate_sampled`, `evaluate_heldout`,
   `evaluate_arms` (each is a reading only: tracker and controller never see them). `evaluate_heldout` collects motion too (`eval.keep_motion_files > 0`) and writes `eval_motion_<env_steps>_heldout_<arena>.npz`. The arms "no_flag", "no_camera", "no_map", "no_memory", "no_goal", "random_goal", "random_cell" and "no_plan" are `evaluation.ablation_chooser` over `_acting`'s chooser (`choose.acting` is its `ActingState`, which "no_memory" resets before every decision).
5. `apply_holds()` (freeze converged classes' adapters and heads and drop them from the sample:
   `trainer.freeze_layouts`);
   the leader sends layout weights (`casting_weights` times `hold_weights`, WEIGHTS) and the replay seeds (REPLAY).
   `casting_weights` is in `evaluation.py:371`: need = standardised shortfall of score (and of
   `layout_sampling.metric`, and of role metrics), `exp(strength * clip(need, -3, 3))`, spread capped at `max_ratio`.

`handle(outcome)` (`:1545`): on ADVANCE the leader writes a `stage.jsonl` line, prints "Stage complete (reason)" and
which classes were not converged, and returns True. A follower returns True on ADVANCE.

`finish(outcome)` (`:2242`): `_save(latest.pt)` (all ranks drain; leader writes), write `finished.json` if there was an
outcome, `progress.write("finished" or "stopped")`, drain, close hub/link, updater, logger, env, ranks.
A run that raises in the loop still saves `latest.pt` here, whatever state the networks are in (including after a NaN).

## The lock-step (`env.py`, `protocol.py`)

The sim is the server and the learner the client. One STEP per decision carries every env's observations; the sim
blocks until the ACT. Frame: `HEADER <II` (type, length). Messages (`protocol.MsgType`): HELLO 1, SPEC 2, STEP 3, ACT 4,
CLOSE 5, MODE 6, WEIGHTS 7, REPLAY 8, DEVICE 9, DEVICE_ACK 10, PROGRESS 11 (12 is unused). `PROTOCOL_VERSION = 28` (the ACT goal section is `GOAL_WIRE_INTS` = 8 int32 an agent: four plan joints, four cell words)
(`protocol.py:14`; the sim's `Bridge/Protocol.h` agrees). `MODE_FLAG_STAND_IN = 2`; bit 1 is unused (`:67-70`).

- Connect: `ForgeEnv._connect` retries every second until `connect_timeout` (600 s), Unix socket or `tcp://host:port`
  (TCP_NODELAY). Send HELLO (version, rank, ranks); receive SPEC (`decode_spec`; version mismatch raises
  `ConnectionError`); optionally receive DEVICE and answer DEVICE_ACK (`_answer_device`, `device.open_buffers`).
- `Spec` (`protocol.py:112`): num_envs, agents_per_env, obs_dim, state_dim, num_actions, episode_info_dim, goal_count,
  tick_ms, decision_ticks, episode_seconds, scenario, layouts, episode_info_names, env_groups, kinematics_dim,
  image_bytes, look_heads, map_bytes, jitter_ms, spike_max_ms, spike_prob (protocol 27; `mean_decision_ms`). `camera_bytes = image_bytes + map_bytes`. `decision_ms = tick_ms *
  decision_ticks`. `env_groups` 2 means half-batch.
- `Spec.step_layout` gives the STEP payload arrays in wire order: obs, state, mask, layout, present, reward, done,
  terminated, final_obs, final_state, episode_info, episode_seed, kinematics, then image and final_image (if
  `image_bytes`), map and final_map (if `map_bytes`). `final_*` and `episode_info` carry only the envs that are done
  (`ENDED_ONLY`); the decoder fills the rest with zeros. `present` is 0, 1 or 2; 2 (`PRESENT_STAND_IN`) is the "human"
  stand-in's row (`Step.stand_in`). With device buffers, obs, state, mask (and image) are tensor views of the sim's
  memory, valid until that group's next STEP.
- `decode_step` copies, checks the length (ValueError becomes `ConnectionError`), joins image and map into one
  camera row. `encode_step`, `decode_act`, `decode_mode*`, `decode_weights`, `decode_replay` exist for the tests and
  the bench's fake sim; the sim itself is C++.
- ACT: header `(env_begin, envs)`, then `int32 actions[E,A]`, goals `[E,A,2]` if the policy has a goal head, look
  `[E,A,heads]` if `spec.look_heads` (a stage with look heads always sends one: `LOOK_HOLD = (3, 2, 0)` for 3 heads).
- MODE (`<IIIIII32s`): evaluate flag, seed base, episodes, flags, first seed, held-out arena (index+1), baseline name.
  Replies with a fresh STEP of every env. WEIGHTS: float32 per layout slot, `MAX_SPECS = 4` per layout. REPLAY: seed
base,
  fraction, then seed list (max 65536). PROGRESS: three float32 (progress, shaping scale, cost scale), clamped to [0,1].
  None of WEIGHTS, REPLAY, PROGRESS gets a reply.
- `ForgeEnv.step` sends every group's ACT before reading (half-batch works as a whole-pool step); `send_act` and
  `receive_step` are the pipelined calls. `close()` sends CLOSE.
- `ClusterEnv` (`env.py:245`): several sims as one pool, laid end to end; the first sim is the host's and is not
  optional. A worker that errors (`OSError`, including a socket timeout of `cluster_timeout`, default 60 s) is dropped:
  its rows come back as `_absent` (no character present, mask action 0 only, zero reward), `sat_out` marks them, and the
  rollout marks those decision rows invalid. `rejoin()` (every 10 s) reconnects only a worker whose SPEC is identical,
  re-sends the last WEIGHTS and REPLAY, and returns its fresh STEP for the rollout to splice in. A
  cluster's evaluation seeds are shared out in proportion to live envs (`_shares`). The workers' sims are opened
  without a device (`ForgeEnv(endpoint, ...)`, no `device=`), so only the host's sim can use device buffers.

`device.py`: `host(x)` copies a tensor to numpy (used everywhere); `DeviceBuffers` opens the sim's HIP IPC handles with
`libamdhip64` found next to torch (ROCm only) and wraps them as tensors; `open_buffers` declines with a reason unless
the
rollouts run on the same GPU under a HIP torch. `animus/__init__.py` sets `HSA_ENABLE_IPC_MODE_LEGACY=0` before torch.
`blas.py`: on gfx12 ROCm only, sends matmuls to rocBLAS with TunableOp, tunings in `var/animus-forge/tunableop`;
`save()` writes the tuning file after each of the first 21 updates (`TUNING_UPDATES = 20`, condition `_saves > 20`) and
then disables tuning.

## Config keys read by `TrainingRun`

Top level: `run_name`, `runs_dir`, `layouts_dir`, `socket`, `cluster_sims`, `cluster_timeout`, `rank`, `ranks`,
`local_rank`, `local_ranks`, `dist_address`, `dist_iface`, `dist_timeout`, `seed`, `total_env_steps`,
`rollout_length`, `log_every`, `checkpoint_every`, `checkpoint_env_steps`, `keep_checkpoints`, `overlap_updates`,
`train_device`, `rollout_device`, `torch_threads`, `init_from`, `seed_from`, `merge_from`, `finetune_from`, and the
sections `mappo` (rank_sync, recurrent_size, gamma, gae_lambda, reference_decision_ms,
foresight_*, goal_*, slow_goal_*, weight_sync_every, entropy_coef, ...), `eval`, `convergence`, `fade`, `costs`,
`entropy_floor`, `layout_sampling`, `distill`, `cast`, `style`, `status`. Defaults:
[py-learner-config.md](py-learner-config.md).

## Tests

No tests (removed 2026-10-07); see [tests.md](tests.md).

## Observed issues

- `train.py:59-64`: constants sit between import groups; `config.py:592-596` puts a method before the `seed` field.
- `train.py:1138-1143`: `checkpointed_env_steps` created lazily; two full serialisations per checkpoint.
- `train.py:1228-1229`: a partner snapshot saves `latest.pt` mid-run and so drains an overlapped update.
- `train.py:215-217`: heldout, sampled, baseline and arm rows in eval.csv carry the learner tracker's margin/best.
- `train.py:1747`: `env_steps` counts frozen, cast, partner and absent rows too, so budgets are in seats played.
- `train.py:2235-2238`: `finish()` saves `latest.pt` even after an exception, whatever the networks hold.
- `train.py:3`, `698`: docstrings still name `stage4_duel`; `train.py:17` says the learner exits 0 either way, true
  only for a clean end.
- `evaluation.py:8`: the module docstring says the score is the mean return; the default is `score_outcome`
  (`:44`, `:78`).
- `protocol.py:1` says the mirror is of `src/Bridge/Protocol.h`; the file is
  `src/server/game/Animus/Bridge/Protocol.h`. `Spec.image_bytes` comment (`:133`) says "x 4"; the pixel is 5 bytes
  (`:21`, `:86`).
- `rewards.py:1-47` docstring and `OUTCOME_TERMS` describe v1 stages (stage 4, druid_dps); the live stages give
  `reward_terms` in stage.json.
- No tests (removed 2026-10-07).
- `episode_means.PER_EVENT` is a hand-kept table of column to count column (`episode_means.py:14-81`); a new per-event
  metric not listed there is averaged over all episodes (a zero for an episode with no event).
