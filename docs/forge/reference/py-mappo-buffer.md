# `mappo/buffer.py`, `mappo/valuenorm.py`

Purpose and scope. Rollout storage and advantage estimation, and the return normaliser.
Overview: [py-mappo.md](py-mappo.md); the consumer is [py-mappo-trainer.md](py-mappo-trainer.md).

## Map table

| Path | Lines | Role |
|---|---|---|
| `apps/forge/python/animus/mappo/buffer.py` | 398 | `compute_gae`, `compute_foresight`, `compute_span_gae`, `decisions_left`, `RolloutBuffer` |
| `apps/forge/python/animus/mappo/valuenorm.py` | 46 | `ValueNorm` |

Tests: `test_gae.py` (hand values, truncation, termination), `slow_gae_reference.py` (a reference used by
`test_two_clock.py`), `test_span_gae.py`, `test_two_clock.py`, `test_foresight.py`, `test_masking.py`
(`test_value_norm_follows_a_drifting_return_scale`),
`test_vision_bytes.py::test_the_rollout_buffer_keeps_the_images_as_bytes`,
`test_mental_map.py::test_the_buffer_keeps_camera_rows_with_their_map_as_bytes`,
`test_free_look.py::test_the_rollout_buffer_holds_the_look`.

## `compute_gae` (`buffer.py:16`)

Inputs `rewards, values, final_values [T,E,A]`, `dones, terminated [T,E]`, `last_values [E,A]`, `gamma, gae_lambda`.
Backward loop over T. The successor value at step t is: `last_values` at the final step else `values[t+1]` when the
episode continued; `final_values[t]` (V of the ended episode's last observation) when it ended **truncated**
(`done & ~terminated`); 0 when it **terminated**. The recursion never crosses a done:
`gae = delta + gamma*lambda*(1-done)*gae`. Returns `(advantages, advantages + values)`. Shapes of `dones` broadcast over
agents. It does not mask invalid (absent-seat) rows: those carry whatever the buffer holds and are excluded later by
`valid` in the update.

## `compute_foresight` (`:55`)

Discounted returns at each of `gammas`, `[T,E,A,H]`, the same recursion with lambda 1; a truncation bootstraps from the
head's own prediction for the ended episode's last state (`final_predictions`), a termination from zero.
`decisions_left(dones) -> [T,E]` (`:133`): decisions until the episode ends, -1 if it does not end in the rollout.

## `compute_span_gae` (`:83`)

GAE on the slow goal clock. A transition runs from a decision where `chosen` to the next chosen decision of the same
agent (or the episode end) and carries every reward between (summed, not discounted, inside the span); discounting is
per
chosen decision. Returns `(advantages, returns, valid)`; `valid[t]` is true only for chosen decisions whose span has an
honest target (it ended with an episode end that terminated, or is followed by another chosen decision). A span cut by
the rollout's end or by a truncated episode is left out. Vectorised over `[E,A]`.

## `RolloutBuffer` (`:144`)

Constructor `(steps, envs, agents, obs_dim, state_dim, num_actions, foresight=0, recurrent=0, goals=False, slow_goal=0,
goal_slots=1, image_bytes=0, look_heads=0)`. All numpy on the host except `obs`, `state`, `mask`, `image`, which move to
the device when the sim writes them there (protocol 15; `_store :259`, `store_rows :246`): the first device tensor moves
that array to its device for good (`setattr`). Fields (shape `[T,E,A,...]` unless noted):

| Field | dtype/shape | Meaning |
|---|---|---|
| `obs` `[..., obs_dim]`, `state [T,E,state_dim]`, `mask [..., N]`, `image [..., image_bytes]` uint8 | | the decision's inputs; image is the camera row (image + map crop) as bytes |
| `layout`, `actions`, `valid` (`False` = a seat without a character) | int64/bool | |
| `log_probs` | float32 | the policy's **joint** log prob (action + look; the goal's is separate) |
| `look [..., look_heads]` int8, `look_log_probs` | | the look choices; the look's part of `log_probs` |
| `values`, `final_values`, `rewards`, `advantages`, `returns` | float32 | `values` are denormalised |
| `dones`, `terminated` `[T,E]` | bool | episode ended / ended by termination (not truncation) |
| `memory`, `critic_memory` `[..., recurrent]` | | the GRU states each decision was taken with |
| `goal`, `goal_log_probs`, `goal_chosen`, `goal_slots [..., goal_slots]`, `achieved` | | goal pair held, the log-prob of choosing it, whether this decision chose it, slots drawn, what the next observation says was achieved |
| `slow_memory`, `slow_values`, `slow_advantages`, `slow_returns`, `slow_valid` | | the slow loop's |
| `foresight_preds`, `final_foresight`, `foresight_targets`, `foresight_valid` | `[..., foresight]` | |

Methods: `add_decision` (`:214`) records a step; `add_outcome` (`:272`) records the result and advances `cursor`;
`finish`
(`:287`) runs GAE (always), span GAE for the slow clock (if `slow_goal` and `slow_goal` size > 0; also narrows
`goal_chosen` to `chosen & valid`), and the foresight targets (if `foresight` and `last_foresight` given), including the
observation targets (`obs_targets`: per target a column per layout, `ahead` decisions, `window` = "at any point");
`sequences()` (`:371`) returns the dict the update replays; `reset()` (`:365`) zeros the cursor;
`mean_reward()`.
Two buffers alternate when updates are overlapped (`train.py:798`). Memory per step is dominated by `obs`
(`E*A*obs_dim*4` bytes) and `image` (`E*A*54784` bytes for M2).
Quirks: `goal_chosen` is rewritten
inside `finish`; the line `# Kept where the sim put them when it put them on the device.` (`buffer.py:13`) is a comment
for a constant that no longer exists.

## `ValueNorm` (`valuenorm.py:9`)

The MAPPO paper's debiased exponential moving average of the returns' mean and mean square: buffers `running_mean`,
`running_mean_sq`, `debiasing_term` (all scalars, persistent). `update(values, ranks)` folds a batch (with ranks: sums
over all ranks in float64) with weight `1 - beta` (`beta` default 0.99); `normalize` = `(x - mean)/sqrt(var)`,
`denormalize` the inverse; `var = max(E[x^2] - mean^2, 1e-2)` (`:23`). The floor 0.01 means returns with a standard
deviation under 0.1 are not scaled up. The trainer holds one trained instance and one rollout copy (synced as a buffer
pair). Saved in the checkpoint as `value_norm`; loaded only if both the config and the checkpoint have one
(`trainer.py:2046`).
Tests: `test_masking.py::test_value_norm_follows_a_drifting_return_scale`. Async learners trade its three buffers as
part
of the flat vector ([py-human-and-misc.md](py-human-and-misc.md), `async_sync`).

## Observed issues

1. Stale comment at `buffer.py:13`.
2. `compute_gae` has no handling of the `valid` mask; correctness depends on invalid rows being excluded downstream.
