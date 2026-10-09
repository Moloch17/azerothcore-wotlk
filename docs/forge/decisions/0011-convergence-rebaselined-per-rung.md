# 0011: Stage convergence is re-baselined at every gate-stepped rung

**Date:** 2026-10-07

**Decision.** A forward step of a gate-stepped ladder starts the overall best, the plateau the learning rate anneals from and each class's convergence over. Such a stage converges only at its top rung. `best_rung<k>.pt` keeps each easier rung's best.

**Reason.** The trackers read the best score ever seen, which is set on the easiest rung, so every harder rung looked like a plateau: M2's lr_scale fell to 0.41 with two rungs left and a class could converge on reaching the top (commits d4a192e7e, 548c18dd1).

**What it constrains.** `ConvergenceController.rebaseline` (animus/stage.py). An older checkpoint above rung 0 drops its convergence state on resume (`stale_ladder`).

See also [principles.md](../principles.md) and the [index](README.md).
