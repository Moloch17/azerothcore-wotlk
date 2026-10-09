# 0022: The overall tracker judges the stage's measure, and forgets on a format change

**Date:** 2026-10-09

**Decision.** The overall `ConvergenceTracker` (it decides `best.pt`, the plateau the learning-rate anneal starts from
and `progress.json` `best_score`) judges `convergence.measure` when the stage defines one, as a share with a binomial
standard error, and the score otherwise. Checkpoints record `judged` and an `evaluation_signature` (`episodes`,
`sampled_every`, `deterministic`, held-out arena names, `replay_fraction`, `judged`); on resume a different value of
either drops the overall and per-class trackers, the convergence history and `plateau_env_steps` (best.pt, the rungs
and the entropy/KL history stay), with a printed reason and an `events.log` line. A checkpoint with no signature is
kept with one printed line. A fine-tune from the stage's own checkpoint also carries its value normaliser.

**Reason.** move2_seek's score stderr is 0.22-0.32 (heavy-tailed returns), so the tracker's margin was 0.6-1.1 against
a best of -1.34 set on 78 frozen episodes; after the 2026-10-09 evaluation-format change (156 sampled episodes) nothing
could beat it until 170M, and a plateau was declared around 110-120M that started the anneal (lr_scale 0.34 by 215M).
A share's margin is about 0.1. Evidence and the limits of the fix are in known-issues.md: found was in fact flat
100-160M, so a plateau at about 120-130M stands under any tracker; what the fix removes is the stale, lucky best and
the score's tails, not the patience rule or the latch.

**What it constrains.** `best.pt` of a stage with a measure is the best by that measure (the return can be lower).
`best_score` in `progress.json` and eval.csv's `best`/`margin` columns are in the measure's units (`best_kind`). Every
checkpoint saved before this resets its trackers once on a stage with a measure.

See also [0011](0011-convergence-rebaselined-per-rung.md), [0013](0013-m2-learning-rate.md) and the [index](README.md).
