# `mappo/trainer.py`: config, rollout path, graphs, the update

Purpose and scope. `apps/forge/python/animus/mappo/trainer.py` (2053 lines) owns the networks, the optimizers, the
rollout-side decision functions (eager and captured as CUDA/HIP graphs), the PPO update that replays whole rollouts
through the GRUs, the slow goal update, and the checkpoint state. Networks:
[py-mappo-networks.md](py-mappo-networks.md).
Buffer, GAE and SIL: [py-mappo-buffer.md](py-mappo-buffer.md). Overview and shapes: [py-mappo.md](py-mappo.md). The
caller is `TrainingRun` in `train.py` ([py-learner.md](py-learner.md)).

## Map table

| Path | Lines | Role |
|---|---|---|
| `apps/forge/python/animus/mappo/trainer.py` | 2053 | `MappoConfig` (41), `ActingState` (209), `_Downloads` (276), `_Decided` (298), `_Packed` (339), `_RolloutGraph` (382), `MappoTrainer` (598) |

Tests: `test_golden_update.py` (CPU update numbers), `test_update_stats.py` (+ `update_stats_reference.json`),
`test_recurrent.py`, `test_masking.py`, `test_goals.py`, `test_goal_queue.py`, `test_two_clock.py`, `test_free_look.py`,
`test_vision_encoder.py`, `test_normalisation.py`, `test_rollout_graph.py` (GPU),
`test_rollout_graph_log.py` (CPU), `test_tick_split.py` (discounts), `test_span_gae.py`, `test_gae.py`.

## `MappoConfig` (`trainer.py:41-198`): every field, its default and who reads it

The yaml section is `mappo:` (`TrainConfig.mappo`, `config.py:657`); unknown keys are handled by
`config.py` (see [config-yaml.md](config-yaml.md)). Checkpoint configs are filtered to known fields by
`evaluate.mappo_from_checkpoint` (`evaluate.py:37`).

| Field | Default | Used by |
|---|---|---|
| `hidden` | (128,128) | network widths; live bases `[256, 512, 512]` |
| `gamma`, `gae_lambda`, `reference_decision_ms` | 0.99, 0.95, 100 | `per_decision` (`:265`) compounds both by `decision_ms / reference_decision_ms`; `train.py:680` |
| `clip`, `value_clip` | 0.2, 0.2 | PPO ratio clip; value clip (in normalised units) |
| `entropy_coef` | 0.01 | initial `trainer.entropy_coef`, then set each update from the controller (`train.py:1733`, `stage.py:526`) |
| `look_entropy_coef` | None | `look_entropy_coef()` (`:1175`): None = follows `entropy_coef`; else `entropy_coef_now * look / entropy_coef` |
| `entropy_final_fraction` | 1.0 | `stage.py:526` (schedule over `total_env_steps`) |
| `goal_entropy_scale`, `goal_entropy_final_fraction` | 1.0, 1.0 | `train.py:1736` sets `goal_entropy_factor` each update |
| `goal_slot_entropy_weight` | 0.1 | assigned onto `GoalHead.slot_entropy_weight` (`:665`) |
| `value_coef` | 1.0 | critic loss weight; also the slow value loss weight (`:1397`) |
| `actor_lr`, `critic_lr`, `lr_final_fraction` | 5e-4, 5e-4, 1.0 | Adam rates; `lr_final_fraction` read by `stage.py:532` (controller's `lr_scale`), applied by `set_learning_rate_scale` (`:789`) |
| `epochs`, `minibatches` | 5, 4 | `_update_recurrent`; `splits = min(minibatches, envs)` (`:1673`) |
| `max_grad_norm` | 0.5 | gradient clipping, each optimizer group separately |
| `use_value_norm`, `value_norm_beta` | True, 0.99 | `ValueNorm` |
| `per_layout_advantages`, `min_layout_rows` | True, 32 | `_normalise_advantages` (`:1546`) |
| `target_kl` | 0.0 (off) | early stop after an epoch when mean epoch KL > `1.5 * target_kl` (`EPOCH_KL_TOLERANCE`, `:1971`) |
| `normalise_observations` | True | `RunningNorm` updates (`:2013`); the networks always contain the norms |
| `recurrent_size` | 0 | GRU width; **must be > 0** (`_update` refuses 0, `:1496`) |
| `chunk_length` | 0 | BPTT chunk (`_chunk_length :1568`: largest divisor of T at most `chunk_length`; 0 when a distiller teaches) |
| `rollout_graphs` | True | `_graphs_off_reason` |
| `vision_chunk_rows` | 0 | `_encode_vision` chunking |
| `map_vin` | False | read by `trainer_inputs` (`train.py:516`), not by the trainer |
| `rank_sync`, `weight_sync_every` | "gradients", 1 | `update()` (`:1469`); also `train.py:549` (async), `async_sync.Link` |
| `goal_count`, `goal_targets`, `goal_every_decisions`, `goal_slots` | 0, 1, 16, 1 | goal head; the goal clock `age % goal_every_decisions == 0` |
| `hindsight_coef` | 0.0 | hindsight imitation term (needs `goal_slots > 1`) |
| `seat_sets`, `entity_attention` | False, False | `seat_sets` selects whether `trainer_inputs` builds descriptors |
| `foresight_coef`, `foresight_horizons_seconds`, `foresight_time_scale_seconds` | 0, (5,30), 60 | foresight head and loss |
| `foresight_obs_targets`, `foresight_feedback` | False, False | extra targets (`FORESIGHT_OBS_TARGETS`, `:203`); feedback of detached predictions |
| `goal_lookahead`, `lookahead_coef` | False, 0.5 | lookahead head and its loss weight |
| `slow_goal_size`, `slow_goal_gamma`, `slow_goal_lambda`, `slow_goal_lr` | 0, 0.993, 0.95, 3e-4 | the slow goal loop; gamma/lambda go to `RolloutBuffer.finish(slow_goal=...)` (`train.py:1710`) |

Removed by the learner trim (`632754d80`): `slow_layout`, `slow_every_decisions`, `slow_gamma`, `slow_gae_lambda`.

## `ActingState` (`:209`)

Per-env, per-agent numpy arrays carried between decisions: `memory`, `critic_memory` `[E,A,R]`, `goal`, `age` `[E,A]`,
`slow_memory` `[E,A,S]`, `queue` `[E,A,goal_slots-2]`, `look` `[E,A,heads]` (int8), `look_log_prob`. `take/put` slice
envs for half-batch acting; `clear(done)` zeroes memories, goal and age, sets queue to -1 for finished envs.
Built by `acting_state(envs, agents)` (`:1150`). `wire_goals` (`:1141`) converts held goals to the `[E,A,2]` int32
(primary, secondary) the ACT carries; `wire_look` (`:1168`) the look choices.

## Rollout path

Entry points: `act` (`:950`, actions only), `act_and_value` (`:964`, the training rollout), `value` (`:1184`,
bootstrap values), `foresight_of` (`:1085`). All run under `_rollout_context()` (the rollout CUDA stream when the
rollout device is CUDA). The rollout uses the **rollout copies** `_rollout_actor/_rollout_critic/_rollout_value_norm`,
synced from the trained networks by `_sync_rollout` (`:843`): copy every paired tensor in place (`_pair_tensors`),
then `fold_normalisation()` on the copies, and on CUDA `densify` + `SharedInputDense`. `update(sync=True)` syncs at
the end; with `overlap_updates` the caller passes `sync=False` and calls `sync_rollout()` when it joins the update.

`act_and_value` returns `(actions, log_probs, values, foresight or None, goals or None)` where goals is
`(goal, goal_log_prob, chosen, slow_before, slow_value, goal_slots)`. Two implementations:

1. **Graph path**: `_rollout_graph` (`:900`) returns a `_RolloutGraph` or None. `graph.run` (`:526`) fills pinned host
   buffers (or copies device-fed inputs), replays, synchronises the rollout stream once and copies the pinned outputs.
2. **Eager path**: `_decide` (`:1012`) computes features, the goal decision (`decide_goals`), the action distribution
   (`Categorical.sample` or argmax), the look, foresight and memory, queueing every result into a `_Downloads`
   object, then `_Decided.finish` (`:313`) updates the `ActingState` from the fetched arrays. The critic is run in
   `act_and_value` itself (`:997`).

Both must agree; `test_rollout_graph.py::test_the_graph_decides_as_the_eager_path_does` (GPU only) is the guard. They
differ in the sampler: graph uses `sample_logits` (Gumbel-max), eager uses `Categorical`.

### Graph capture conditions (`_graphs_off_reason`, `:883`)

Graphs are used iff all hold, else a one-time log line names the reason (`announce_graph`, `:371`, tests
`test_rollout_graph_log.py`):

1. `_rollout_stream is not None`, i.e. `rollout_device.type == "cuda"` (`:680`; HIP shows as cuda);
2. `config.rollout_graphs` is true;
3. the call has an acting `state` (so `act` calls and evaluation calls without one run eager);
4. `self.seat_sets is None` (seat sets branch on the host).

One graph per key `(envs, agents, obs width, mask width, state width, deterministic, device_fed)` (`:912`), captured on
first use: two warm-up runs on the capture stream, then `torch.cuda.graph` of `_body` (`:425-432`). Inputs live in
two `_Packed` buffers (a pinned host buffer and a device buffer with typed views, one copy each way): `large`
(obs, mask, state, image) and `inputs` (layout, memories, goal, chosen, queue, slow memory). Outputs are laid out by
the first warm-up and downloaded in one copy (`:518-524`). The graph reads the rollout networks' tensors at their
captured addresses; `_sync_rollout`/`DenseLayouts.refresh` copy in place so a replay always sees the latest weights.
A camera does **not** disable graphs. The graph body is `_body` (`:434`): the critic's state encoding, the shared dense
adapters, the camera (once, for both networks), actor features, the goal decision, the critic value (denormalised), the
action logits, the sample, the look, foresight and memory outputs.

### The one place the learner trim edited (commit `632754d80`)

The commit message of `632754d80` states the trim touched rollout-graph code in three places, with "the captured body
and the `_Packed` layouts untouched":

- `_graphs_apply`: dropped the terms `self.slow_layout >= 0` and `self.director is not None`. **That three-term
  expression is no longer in the code.** A later commit (`4e45ed18c`, "log once when a rollout graph is first
  captured...") split the decision into `_graphs_off_reason` (`:883`, returns the reason string) and left
  `_graphs_apply` (`:897`) as `return self._graphs_off_reason(state) is None`. Production code calls
  `_graphs_off_reason` through `_rollout_graph` (`:905`); `_graphs_apply` is called only by tests
  (`test_vision_encoder.py:468`, `test_sight.py:225`, `test_mental_map.py:235`).
- `_RolloutGraph.run` now returns the five-value tuple (`:585`) instead of six (the sixth, `chosen`, was the slow
  layout's).
- `_Decided.finish` now returns four values (`:336`); `_decide` and `act` unpack four (`:960`), `act_and_value` unpacks
  four (`:1006`).

Only GPU tests cover these (they skip on CPU), so the edit was verified on CPU only by the golden test, which uses the
eager path (`test_golden_update.py` builds the trainer with `device="cpu"`). Residue of the trim still visible:
`_Decided.__init__` takes `envs, agents` it does not use and sets `goal_chosen = None` (`:305`), never read.

## The update

`update(buffer, auxiliary=None, sync=True)` (`:1466`): validates `rank_sync` in {"gradients","weights","async"}, calls
`_update` (-> `_update_recurrent`), then `_update_goals` if `slow_goal_size`, then in "weights" mode averages the
parameters every `weight_sync_every` updates (`:1479-1490`) and syncs the rollout copies. `auxiliary` must have
`sequence_loss` (the `Distiller`, [py-learner.md](py-learner.md)); a per-minibatch hook is refused (`:1499`).

`_update_recurrent` (`:1603-2025`), step by step:

1. Stats accumulators.
2. `host = buffer.sequences()` -> device tensors `data` (`:1637`). With hindsight, `achieved` is recomputed on the
   device
   from the observations (`_achieved_of`, `:1126`). If `chunk_length` applies, arrays are re-cut with `chunked` (`:25`).
3. Advantages normalised on valid rows, **per layout** when the layout has at least `min_layout_rows` rows else by the
   rollout's statistics (`_normalise_advantages`). `ValueNorm.update(returns)` (all ranks), `returns_target` and
   `old_values` normalised.
4. `explained_variance` computed from the denormalised returns/values (`:1667`).
5. Loop `epochs` x minibatches (minibatch = a random subset of envs; `torch.randperm` on the host so the layout groups
   and GRU pieces are cut host-side, `:1683-1703`):
   - camera embedding encoded **once** (`_encode_vision`), handed to both networks as a leaf `seen_leaf`;
   - actor: `encode` -> `carry` through the GRU (memory = the one stored at step 0 of each env) ->
     `action_distribution`;
     `log_probs`, entropies; without a slow loop, the goal head's log-prob (times `goal_chosen`) joins the ratio and its
     entropy (times `goal_entropy_factor`) joins the entropy; the look head's joint log-prob and entropy join;
   - the taught loss from `auxiliary.sequence_loss` if a distiller is teaching;
   - `ratio = exp(log_probs - taken)`, where `taken` is the stored joint log-prob (plus the stored goal log-prob on the
     same condition);
6. Loss terms, with their coefficients:

| Term | Formula (code) | Coefficient / key |
|---|---|---|
| policy | `-mean(min(ratio*A, clip(ratio, 1-clip, 1+clip)*A))` over valid rows (`:1780`) | `clip` 0.2 |
| entropy (actions + goal if no slow loop) | `- entropy_coef * entropy` (`:1784`) | `entropy_coef` now (controller schedule x floor) |
| look entropy | `- look_entropy_coef() * look_entropy` over rows with a camera (`:1787`) | `look_entropy_coef` |
| foresight | smooth-L1 on the discounted-return horizons, squared error on the episode-left share and on the observation targets, masked by `foresight_valid` (`:1793-1799`) | `foresight_coef` |
| hindsight | `-mean log pi(a | achieved goal)` over relabelled rows (`:1841`) | `hindsight_coef` |
| distillation | `distill_loss` as returned (already a mean) (`:1865`) | `auxiliary.coef` |
| value (critic) | `max((V - R)^2, (V_clipped - R)^2)` mean, with `V_clipped = V_old + clamp(V - V_old, +-value_clip)`, all in normalised units (`:1906-1908`) | `value_coef` |

   Actor backward; `vision_grad_actor` recorded; gradients averaged across ranks (rank_sync "gradients"); clipped by
   `max_grad_norm` over `_actor_parameters()` (all actor parameters except the slow loop's and the camera's); step
   `actor_opt`. The critic then runs on a second CUDA stream (`_update_streams`, two streams created at `:683`),
   replaying its own GRU; backward; step `critic_opt`; the camera's combined gradient goes back through the encoder
   (`_backward_vision`) and `vision_opt` steps once, clipped separately.
7. After the epoch, `target_kl` early stop on the mean of the ranks' KL.
8. Post-loop: layout stats (`_finish_layout_stats`), goal stats, look stats; and only now are the observation
   normalisers updated (`update_norms`, `state_norm.update`) so the epoch-0 ratio is 1 (`:2013`); then the rollout sync.

Reported statistics (`stats`): `policy_loss, value_loss, entropy` (actions only), `clip_frac, approx_kl, approx_kl_move`
(the movement action alone, stored joint log-prob less the look's part), `actor_grad_norm, critic_grad_norm`,
`goal_entropy` (if goals), `look_entropy`, `vision_grad_{norm,actor,critic}`, `foresight_loss` and
`forecast_*` qualities, `explained_variance`, `epochs_run`, `epochs_done` (a duplicate of `epochs_run`, `:1979-1982`),
`minibatches_done`, `goal_<k>_share`, `goal_targeted_share`, `goal_kept_share`, `goal_swap_action_change`, `look_*`
shares, `hindsight_*`, `update_compute_seconds`, `weight_sync_seconds`. Names are consumed by metrics/CSV
([metrics.md](metrics.md)).

Per-layout statistics: `layout_stats[i] = {entropy, approx_kl, rows}` per layout (action entropy only), read by
`stage.py` for per-class convergence. `freeze_layouts(indices)` (`:753`) turns off `requires_grad` for the adapters and
heads of converged classes (in both networks); only the trunk can still move them. Note `requires_grad_(False)`
parameters keep their Adam state; `reset_optimizers` clears `frozen_layouts`.

### Value normalisation and GAE

The buffer stores denormalised values; `ValueNorm` (46 lines, see [py-mappo-buffer.md](py-mappo-buffer.md)) normalises
returns and old values at update time. GAE is computed in `RolloutBuffer.finish` with the per-decision `gamma` and
`lambda` from `per_decision`.

### Entropy schedules

Three separate factors multiply: the controller's `entropy_coef(env_steps)` = `mappo.entropy_coef * entropy_scale *
schedule(entropy_final_fraction)` (plus an entropy floor / boost, `stage.py:526`, other agent); the goal head's
`goal_entropy_factor = goal_entropy_scale * schedule(goal_entropy_final_fraction)` (`train.py:1736`); the look head's
share is a constant ratio of the action coefficient. `schedule(final, steps, total)` (`:259`) is linear and clamps at
`final` past `total_env_steps`. The learning-rate scale is the controller's (`lr_scale`), held at 1 until the first
plateau ([principles](../principles.md) 10; `stage.py:532`).

### Minibatching, chunking, vision chunking

Minibatches are envs, never rows (the GRU needs whole sequences). With `chunk_length` L the T x E rollout becomes L x
(T/L * E) "envs", each replayed from the memory stored at its first step: gradient does not cross a chunk start and the
start memory came from the old policy (documented at `:103-110`). Vision chunking (`vision_chunk_rows`) encodes the
camera in chunks without a graph, then re-encodes each chunk with a graph to backpropagate, trading roughly 1.7x encoder
time for memory (comment `move1_controls.yaml:51`).

## The slow goal update (`_update_goals`, `:1277`)

Runs after the main update when `slow_goal_size > 0`. Gathers each seat's goal-choice decisions (`goal_chosen & valid`),
replays them in order through the slow GRU (`_carry_sequence` over `actor.slow_memory`, cleared where an episode ended
between two choices), PPO-steps the goal head on the span GAE advantages (`buffer.slow_advantages`, only choices whose
span has a target, `slow_valid`), the slow value on `slow_returns`, and, with lookahead, the success (BCE) and duration
(squared error) heads on what the chosen goals did (`reached` from the next observation's goal block, duration in
fractions of a minute = 240 decisions). Loss: `policy - entropy_coef*goal_entropy_factor*entropy + value_coef*value_loss
(+ lookahead_coef*lookahead_loss)`; its own optimizer `slow_opt` (lr `slow_goal_lr`, params = slow GRU, goal head, slow
value) with ranks averaging. With nothing to learn and ranks in "gradients" mode it still steps epoch times so
collectives line up (`:1285-1292`). It uses the fast features detached, under `no_grad` (`:1367`). It reads the camera
for features through `_encode_vision` and `actor.encode(..., vision_embedding=seen, image=image)`.
Config keys: `slow_goal_*`, `lookahead_coef`, `goal_*`, `epochs`, `clip`, `value_coef`, `max_grad_norm`.

## Checkpoint state (`:2027-2053`)

`state_dict()`: `actor`, `critic`, `value_norm`, `actor_opt`, `critic_opt`, `vision_opt` (if present). **`slow_opt` is
not saved.** `load_state_dict(state, load_optimizers=True)`: `load_actor_state` (tolerates only blind-column masks and
the
goal-scale tables), critic strict apart from blind-column masks, value norm if both sides have one, optimizers if asked
(`vision_opt` only if present in the checkpoint), then `_sync_rollout`. The optimizer state loads by parameter order
within each group; a change that adds, removes or reorders actor parameters breaks loading of `actor_opt` (UNVERIFIED
what the failure looks like; torch raises on a group size mismatch).

## Observed issues

1. `slow_opt` neither saved nor learning-rate-scaled (see [py-mappo.md](py-mappo.md) issues 1-2).
2. `_update_recurrent` docstring says the critic is feed-forward; it is recurrent (`:1608` vs `:1899`).
3. `_graphs_apply` kept only for tests; two names for one decision.
4. `director_columns_clear` name.
5. `epochs_run` and `epochs_done` duplicate each other (`:1979-1982`).
6. `_Decided` carries unused `goal_chosen`, `envs`, `agents`.
7. `_updates_since_sync` set via `getattr` (`:1480`).
8. `reset_optimizers` also resets `layout_stats` and `frozen_layouts` (`:750-751`): it is called only from `__init__`
   (checked by grep), so a stage restart never resets Adam state in the live code even though the docstring says "after
   a restart".
10. The CPU path of the update leaves `_masked_stats` False (`:637`) so hindsight uses picked rows there and masked
    arithmetic on CUDA: two code paths with one test for equivalence (`test_update_stats.py` runs the masked path on
    CPU through the flag).
11. `value()` and `_decide` call `_groups`, which returns None on CUDA (dense rollout copies) and a grouping on CPU:
    the eager and graph paths differ in which adapters run (per-layout loop vs `DenseLayouts`).
12. `act()` always passes `state` into `_decide` but ignores graphs: `act` is eager even on the GPU.

## Reviewer notes

- Questions: why not save `slow_opt`? Should `set_learning_rate_scale` include `slow_opt`? Is `chunked` + `target_kl`
  with
  `approx_kl_move` consistent (chunk starts are off-policy for memory)? `explained_variance` uses the denormalised
  buffer values versus returns: fine, but it ignores invalid rows only via `rows`.
- The update is 420 lines in one method; the natural cuts are: data prep, per-minibatch actor half, critic half, stats.
  Protect it with `test_golden_update.py` and `test_update_stats.py` before touching it, and re-run the GPU tests
  (`forgectl test --gpu`, [forgectl.md](../forgectl.md)) for the stream/graph changes.
- Because the learner imports modules lazily, changing files under a running learner can mix versions (known fact);
  `trainer.py` imports `.networks` at import time, but `load_state_dict` imports `.networks` names lazily
  (`:2039`, `:838`).
