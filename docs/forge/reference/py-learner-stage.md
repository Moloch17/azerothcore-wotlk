# Python learner: the stage lifecycle and the convergence rule

Part of [py-learner.md](py-learner.md). Covers `apps/forge/python/animus/stage.py` (829 lines) and the
`ConvergenceTracker` in `evaluation.py:622-726`. Line numbers are those of commit `bd32b9dc8`. The training loop that
calls these is in [py-learner-train.md](py-learner-train.md); the config fields they read are in
[py-learner-config.md](py-learner-config.md). Related: [py-mappo.md](py-mappo.md) (`schedule`, `freeze_layouts`),
[stages.md](stages.md), [glossary.md](glossary.md), [known-issues.md](known-issues.md).

There are no pass gates. A stage ends when every class the run played has `converged` (and the shaping ladder is
settled and the cost ladder ready), or when `total_env_steps` is reached (`stage.py:727-738`). Nothing else ends it.

## Three things called a ladder, two called a stall

- The **shaping ladder** (`ShapingFade`, config `fade`) and the **cost ladder** (`CostLadder`, config `costs`) are
  learner-side. They scale reward terms. Both are described below.
- The **difficulty ladder** (the sim's rungs: M2 placements, the drills, the dungeon rungs) is sim-side. The learner
  only reads it through the episode info columns `difficulty` and `at_top_rung` (`train.py:900-902`, `1995-2017`).
  The "wing ladder never steps back and has no stall alarm" known fact is about this sim-side ladder
  (`docs/forge/deploy-gate.md`, "Before the dungeon stages train (a)"). The learner's own `ShapingFade` does have
  a stall alarm (`stage.py:314-335`).
- Two unrelated "stall" messages exist in the learner: the ladder gate stall (`ShapingFade._watch_stall`,
  `stage.py:314`) and the KL stall print in `TrainingRun.audit_progress` (`train.py:2097-2125`, constants
  `train.py:59-63`). The second is only a console line.

## ConvergenceTracker (`evaluation.py:622-726`)

A `@dataclass` with `patience=0, window=4, z=2.0, min_improvement=0.02, min_improvement_abs=0.01`, and state `best`,
`best_stderr`, `best_env_steps`, `evals_since_best`, `last_margin`, `history: list[(env_steps, score, stderr)]`.

- `margin(stderr)` (`:647`): 0 if no best yet, else `max(min_improvement_abs, min_improvement * |best|,
  z * sqrt(stderr^2 + best_stderr^2))`.
- `observe(score, env_steps, stderr)` (`:653`): appends to `history`, sets `last_margin`, and is a new best when there
  is no best or `score > best + margin`. A new best resets `evals_since_best`; otherwise it increments.
- `projected_gain()` (`:666`): over the last `max(3, window)` history points, the least-squares slope times the mean
  spacing times `patience`. None below 3 points or if all env_steps are equal.
- `converged()` (`:679`): False if `patience <= 0` or `evals_since_best < patience`. Then
  True if there is no projected gain, else `gain <= margin(mean stderr of the recent points)`.
- `forget_scores` (`:708`) clears best, history and margin. `load_state_dict` (`:719`) uses `.get` with defaults and
  does not restore `last_margin` (it stays 0 until the next `observe`).
- `state_dict` (`:699`) saves best, best_stderr, best_env_steps, evals_since_best, history.

Who owns a tracker:

| Tracker | Built at | patience | Decides |
|---|---|---|---|
| `ConvergenceController.tracker` | `stage.py:468` | `convergence.patience` if evaluating else 0 | `best.pt`; `plateau_env_steps` (learning-rate anneal) |
| each `LayoutState.tracker` | `stage.py:470` | `convergence.window` | that class's score plateau |
| `ShapingFade.tracker`, `CostLadder.tracker` | `stage.py:183-186` | the ladder's `window` | the ladder's own plateau (only when it steps on a plateau) |

## LayoutState and `missing` (`stage.py:61-118`)

One per layout name (class). Accumulators since the last evaluation: `kl_sum`, `entropy_sum`, `updates`, `rung_sum`,
`rung_count`, `top_sum`, `top_count`. Per-evaluation lists: `kl`, `entropy`, `rung`, `top`, `scores`. Flags: `played`,
`ladder`, `top_scored`, `converged`, `converged_score`, `converged_margin`, `reentries`.

`missing(config, evals_needed)` returns the unmet signals; empty means converged. `evals_needed` is
`convergence.window` everywhere. In code order:

1. `score`: fewer than `window` scores, or `tracker.converged(0, 0)` false (patience `window`, so: no new best for
   `window` evaluations and the trend projected `window` evaluations ahead does not beat the margin).
2. `kl`: fewer than `window` interval means, or any of the last `window` above `convergence.kl` (0.003). Each interval
   mean is the mean over the updates in that interval of `approx_kl / lr_scale` (`observe_update`, `:499`).
3. `entropy`: fewer than `window` values; or the absolute slope of a degree-1 `np.polyfit` of the last `window`
   values of `entropy / ln(allowed actions)` exceeds `convergence.entropy_slope` (0.01); or, with
   `entropy_floor.fraction > 0`, the latest value is below that fraction.
4. `ladder`: fewer than `window` rung entries in the list at all (`len(self.rung)`, not the window), or the max-min of
   the non-None rung means in the last `window` exceeds `RUNG_SETTLED = 0.5`. A stage with no `difficulty` column has
   `None` entries and is never blocked after `window` evaluations.
5. `top_rung` (only if `convergence.top_rung` and `self.ladder`): blocked unless the latest evaluation scored the class
   at the top rung (`top_scored`); and if the sim reports `at_top_rung` at all, the last `window` interval shares must
   all be present and at least `TOP_RUNG_SHARE = 0.9`.

The module docstring (`stage.py:8`) says "all four"; it lists five. The code has five.

## ConvergenceController.observe, in code order (`stage.py:559-636`)

Called once per learner evaluation on the leader (`train.py:1265`). `summary` is `EvalResult.summary(report)`.

1. `evals += 1`; remember `before_rungs = _gate_rungs()`.
2. `improved = tracker.observe(summary["score"], env_steps, summary.get("stderr", 0))`; if improved,
   `best_summary = summary`. The return value tells the caller to save `best.pt`.
3. `costs_ready = costs.ready`. `anneal_ready = costs_ready and (fade.require_plateau or fade.settled)`.
4. If `plateau_env_steps is None and anneal_ready and tracker.converged(env_steps, 0)`: `plateau_env_steps =
   env_steps`. This is the moment the learning rate may begin to anneal (see `lr_scale`). With
   `convergence.patience = 0` the overall tracker never converges, so the plateau is never set and `lr_scale` stays 1.
5. `anneal_starting = (plateau_env_steps == env_steps)`; `ladders_settled = self.ladders_settled()` (at most
   `fade.moving_classes` played classes have a rung range above 0.5 over the window).
6. `costs.see_gate(summary)`, `fade.see_gate(summary)` (reads `gate_metric`, computes its standard error).
7. `costs_message = costs.observe(score, stderr, env_steps, ladders_settled, anneal_starting)`. The cost ladder moves
   first. `fade_message = fade.observe(..., ladders_settled and costs_ready and costs_message is None,
   anneal_starting)`: the shaping ladder does not step in the same evaluation the cost ladder moved, nor while the
   cost ladder is not ready, nor while classes' difficulty ladders move, nor at the evaluation the anneal starts.
8. Per class (`:592-630`): take the interval means and reset the accumulators; skip the class if its summary row has no
   score (it then does not count as played). Otherwise `played = True`; append `kl`, `entropy`, `rung`, `top`; choose
   the judged row: the `top_rung` row if `state.ladder and top is not None`, else the plain row;
   `top_scored = judged has a score`; if not scored, skip the rest (no score is added). Otherwise
   `score, stderr = _judged_score(judged)` (the `convergence.measure` column, with a binomial standard error
   `sqrt(max(p(1-p), 1e-4)/episodes)` when it is a share in [0,1], else the row's `score`/`stderr`), append the score,
   `state.tracker.observe`.
   - If already converged: re-entry when `score < converged_score - converged_margin`. The class's tracker and lists
     restart from this one evaluation, `converged = False`, `reentries += 1`.
   - Else if `anneal_ready` and `missing()` is empty: `converged = True`, `converged_score = score`,
     `converged_margin = tracker.margin(stderr)`.
9. `rung_exits`: every gate-stepped ladder whose `_gate_rungs()` value rose gives `(name, old_rung)`. If any,
   `rebaseline()`. The caller then archives `best.pt` to `best_rung<k>.pt` for each exit (`train.py:1298-1301`,
   `runs.archive_rung_best`).

Note that the class convergence at step 8 is not gated on the shaping ladder being at its last rung when
`fade.require_plateau` is true (only `anneal_ready`, which allows it). `after_eval` does require `fade.settled`.

### Class state versus overall state

The overall tracker, `plateau_env_steps` and `best_summary` are the "stage-level convergence state". The classes'
trackers, scores and `converged` flags are the "class state". `rebaseline` restarts both.

## `rebaseline`, `_gate_rungs`, `baselined`, `stale_ladder`

- `_gate_rungs()` (`:638`): `{"fade": rung or 0, "costs": rung or 0}`; a ladder counts only if its `require_plateau` is
  False (a gate-stepped ladder). `CostLadderConfig` has no `require_plateau` field, and `ShapingFade.__init__` reads it
  with `getattr(fade, "require_plateau", True)` (`:151`), so the cost ladder is never gate-stepped: its entry is always
  0, it is never re-baselined, and the collapse and stall alarms skip it (`:301`, `:320`).
- `rebaseline()` (`:644`): `tracker.forget_scores()` in place (the trainer holds the object), `best_summary = None`,
  `plateau_env_steps = None`, and for every class a fresh tracker, empty `scores`, `converged = False`,
  `converged_score = None`, `converged_margin = 0`, `reentries = 0`. It keeps `kl`, `entropy`, `rung`, `top`, `played`,
  `ladder`, the entropy floor scale and the ladders themselves. Then `baselined = _gate_rungs()`. Reason: a harder rung
  scores lower by design, so the easier rung's best made every step read as a plateau and let the learning rate anneal
  early (M2, 2026-10-07, comment at `:647-652`).
- `baselined` is saved in `state_dict` (`:767`). On load (`:803-818`): if any gate-stepped ladder is above the rung
  stored in `baselined` (a checkpoint from before the re-baselining), the controller re-baselines and sets
  `stale_ladder = True`; `restore_evaluation_state` (`:428`) then also clears the overall tracker and the controller's
  tracker. Else, if the checkpoint has no `baselined` key and the fade is gate-stepped and not yet settled, the plateau
  and the classes' convergence are cleared (the old code let them converge at rung 0).
- If `costs.reshaped` (the saved cost rung was past the configured rungs) the plateau and all classes' convergence,
  trackers and scores restart (`:819-829`).

## `ShapingFade` (`stage.py:136-379`), the shaping ladder

Constructed with the config's `fade` block (`CostLadder` passes `costs`). Fields: `enabled = fade.enabled and rungs`,
`gate_metric`, `gate_value`, `require_plateau = fade.require_plateau or not gate_metric`, `rungs`, `window`,
`regress_z`, `give_up`, `stall_evals` (default 4), `stall_env_steps` (default 20,000,000). `scale` is
`rungs[rung]` when enabled, else 1.0.

`observe(score, stderr, env_steps, ladders_settled, anneal_starting)` returns a log line or None, in this order
(`:211-257`):

1. Return None when disabled.
2. `tracker.observe`, `evals_at_rung += 1`, `_watch_collapse()`, `_watch_stall(env_steps)`.
3. Regression (only if `regresses`, i.e. `require_plateau` true, and `step_score` is set, `rung > 0` and
   `evals_at_rung >= window`): over the last `window` scores, `mean` and `mean_stderr`; if
   `mean < step_score - regress_z * sqrt(mean_stderr^2 + step_stderr^2)` the ladder steps back one rung
   (`falls[new rung] += 1`, `_moved()`, `step_score = None`).
4. Step down (`rung < last`, not `held`, `ladders_settled`, not `anneal_starting`, and `_earned`): `rung += 1`,
   `step_score = score`, `_moved()`.
   `_earned`: with `require_plateau` true, `evals_at_rung >= window` and the rung's tracker `converged(env_steps, 0)`
   and `_gated()`; with it false, only `evals_at_rung >= 1` and `_gated()` (the gate reading at or above `gate_value`,
   or no gate).

`settled` is true when disabled, at the last rung, or `held` (`regresses` and `falls[rung] >= give_up`). A gate-stepped
ladder never regresses, so it is settled only at its last rung. `_moved()` resets the rung's wait, tracker,
`lower_gate = gate_seen`, `rung_gates`, `collapsed`, and the stall best/state.

### The collapse alarm (`:292-312`)

Gate-stepped ladders only, not at rung 0, and only when a gate reading exists. It appends the gate reading to
`rung_gates` and compares the last `COLLAPSE_EVALS = 3` readings with `floor = max(COLLAPSE_FLOOR = 0.1,
COLLAPSE_SHARE = 0.25 * lower_gate)` where `lower_gate` is the last reading at the rung below. All three under the floor
sets `collapsed`; the line is returned once, when it first becomes true (`alarm`). It does nothing else.

### The stall alarm (`:314-335`)

Gate-stepped ladders only, not at the last rung, with a gate reading. A reading beats the rung's best only if it is
greater than `gate_best + gate_stderr` (`gate_stderr` from `gate_stderr()`: the summary's `<metric>_stderr`, else a
binomial estimate over the summary's `episodes`, else 0.02). Otherwise `evals_since_gate_best` increases. It is stalled
when that reaches `stall_evals` (4) and the env steps since the best reach `stall_env_steps` (20M; 0 means the
evaluations alone). One line when it first becomes true; `stalled` stays until the best improves. A warning only.
Both alarm lines are printed by `TrainingRun.evaluate` (`train.py:1288-1291`) and the flags go to metrics.csv as
`ladder_collapsed`, `ladder_stalled` (the rung, else -1; `train.py:2035-2038`). The ladder's state includes both.

### Persistence

`state_dict` (`:353`) saves rung, tracker, evals_at_rung, step_score, step_stderr, falls, steps, rung_gates,
lower_gate, collapsed, gate_best, gate_best_steps, evals_since_gate_best, stalled. `load_state_dict` (`:361`) reads
every
key with a default, and clamps `rung` to the configured last rung without comment (the cost ladder instead flags
`reshaped`, below). `forget_scores` (`:342`) keeps the rung and drops the scores.

## `CostLadder(ShapingFade)` (`stage.py:382-425`)

Same machine over `config.costs`, with `NAME "cost ladder"`. Overrides:

- `_earned`: at rung 0 with a `gate_metric`, only the gate; at later rungs only a plateau (`waited >= window` and the
  tracker converged). It ignores `require_plateau`.
- `_why`, text only.
- `ready` (property): disabled, or `settled and evals_at_rung >= window`. The controller uses it to hold back both the
  anneal and the shaping ladder and class convergence's `anneal_ready`, and `after_eval` requires it.
- `load_state_dict` sets `reshaped` when the saved rung is past the configured end (the rungs were shortened) and then
  restarts its own wait, tracker and step score.

## Learning rate, entropy, and the floor

- `lr_scale(env_steps)` (`:529`): if `lr_hold_until_plateau` is off or the run does not evaluate, the plain linear
  `schedule(mappo.lr_final_fraction, env_steps, total_env_steps)` (`mappo/trainer.py:259`). Else 1.0 until
  `plateau_env_steps` is set, then the schedule restarted at the plateau: `schedule(final, env_steps - plateau,
  max(1, total - plateau))`. Used in `rollout()` (`train.py:1720`, applied by `set_learning_rate_scale`).
- `entropy_coef(env_steps)` (`:524`): `mappo.entropy_coef * entropy_scale * schedule(entropy_final_fraction, ...)`.
- `observe_entropy(entropy, allowed)` (`:540`), every logged update: with `entropy_floor.fraction > 0` and more than one
  allowed action, the target is `fraction * ln(allowed)`. If entropy is below the target, `wanted = min(max_boost,
  entropy_scale * 1.5)`, else 1. `entropy_scale += rate * (wanted - entropy_scale)`, clamped to `[1, max(1,
max_boost)]`.
  A floor, never a ceiling.
- `hold_weights()`: per class `convergence.hold_share` (0.02) if converged, else 1. Sent to the sim with the layout
  weights (`train.py:1521-1533`).

## Decisions

- `after_eval(env_steps)` (`:727`): ADVANCE with reason "converged" when `convergence.advance`, at least one class
  played,
  `evals >= window`, every played class converged, `fade.settled` and `costs.ready`. Else CONTINUE. `Outcome.report`
  is `report()`: per class converged, reentries, missing, last score, kl, entropy, rung, top_rung ("never played" for a
  class without rows).
- `at_budget()` (`:735`): always ADVANCE with reason "budget", even with `convergence.advance: false`.
- A class never played is not waited for (`played` is set only when its row has a score).

## `restore_evaluation_state` (`stage.py:428-457`)

Called by `_load_or_seed` on resume (`train.py:969`). Loads `checkpoint["convergence"]` into the tracker and
`checkpoint["controller"]` into the controller. If `controller.stale_ladder` it also clears both trackers. If the saved
`score_kind` (default "" for old checkpoints) differs from the run's, it calls `tracker.forget_scores()` and
`controller.forget_scores()` (best.pt stays on disk; the next evaluation is the new best). The controller's own
`tracker` is the same object as the `TrainingRun.tracker` (`train.py:701`), so after `stale_ladder` and a kind change
`forget_scores` runs twice on the same object (harmless).
`controller.forget_scores` (`:744`) keeps KL, entropy and rung lists and the plateau, and clears best, baseline summary,
the ladders' scores and each class's tracker, scores and convergence.

## Config keys read here

`convergence.*` (advance, patience, window, z, min_improvement, min_improvement_abs, kl, entropy_slope, hold_share,
lr_hold_until_plateau, top_rung, measure), `fade.*` and `costs.*`, `entropy_floor.*`, `mappo.entropy_coef`,
`mappo.entropy_final_fraction`, `mappo.lr_final_fraction`, `total_env_steps`, `eval.every_env_steps`. Defaults:
[py-learner-config.md](py-learner-config.md).

## Tests

No tests (removed 2026-10-07); see [tests.md](tests.md).

## Observed issues

- `stage.py:8` says "all four" signals and lists five.
- `CostLadder` can never be gate-stepped (no `require_plateau` in `CostLadderConfig`), so `baselined["costs"]` never
  moves
  and its rung steps are never re-baselined, archived (`best_<ladder>_rung<k>.pt`) or watched by an alarm. The
  `rung_best_name` branch for non-fade ladders (`runs.py:240`) is therefore unreachable today.
- Fade and cost ladders treat a shortened rung list differently: the cost ladder resets and flags it (`:397-408`), the
  fade
  silently clamps (`:364`).
- No stall alarm at the top rung (`:320`); the collapse alarm skips rung 0 (`:301`).
- `convergence.patience = 0` is described by the sim as making a budget a budget (`ForgeConfig.cpp:725-729`), but
  nothing reads the overall tracker's `converged` to end a stage: stage end is `after_eval` (class convergence), which
  uses class trackers with patience `window`. Patience 0 only keeps `plateau_env_steps` unset, so the learning rate
  stays at 1. UNVERIFIED by test: check whether a fast run (`--set convergence.patience=0`) can still end early by
  class convergence.
- `ForgeConfig.cpp:701-702` says the learner refuses a gated stage without an evaluation interval; no such check was
  found in `train.py` or `config.py` (UNVERIFIED: search once more).
- With `fade.require_plateau` true, classes can converge and be held out of the draw at an intermediate shaping rung
  (`anneal_ready` allows it).
- `evaluation.py:441` cites `animus.gates.noise_allowance`, which does not exist.
- Stale names in comments: `stage9_deadmines` (`config.py:149`), `stage4`, `stage3_rotation` (`config.py:197`).
