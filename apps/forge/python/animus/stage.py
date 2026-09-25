"""When a curriculum stage is finished: one convergence rule for every stage, and nothing else.

The queue (AnimusForge.Queue) moves on to the next stage when the learner exits cleanly, and the learner exits when
the stage has **converged** -- or when it reaches total_env_steps, which is a ceiling and not a target. There are no
pass gates: no metric floors, no baseline comparisons, no restarts, no confirmation runs. What a stage taught is
reported (eval.jsonl, the finished report), never judged.

A stage has converged when its **weakest class** has, and a class has converged when all four of these hold over
the last ``convergence.window`` evaluations:

1. **Score plateau** -- its evaluation score has stopped improving by the margin (ConvergenceTracker per class:
   the larger of a fraction of the best, an absolute step and z standard errors; the last `window` scores' trend
   flat too).
2. **Policy stopped moving** -- its approx_kl per update, divided by the learning-rate scale in force, has stayed
   under ``convergence.kl``. The division matters: the learning rate anneals to a tenth by the budget, and a KL
   that fell with it read as convergence when it was the schedule (the party run: 0.021 to 0.003 over 120M steps
   with the entropy flat the whole way). The rates are also held at full until the overall score first plateaus
   (``lr_hold_until_plateau``), so the KL means what it says for as long as the policy is still finding things.
3. **Entropy settled** -- its entropy over ln(allowed actions) has a slope within ``convergence.entropy_slope`` per
   evaluation, and sits above the entropy floor's fraction when one is set: not still exploring, not collapsed.
4. **The ladder settled** -- on a ladder stage, its training rung (the mean ``difficulty`` of its training
   episodes) has not moved by half a rung; on a league stage, the live policy's win rate against the hardest league
   member has moved by less than 0.05. A stage with neither has nothing to settle.

A class that has converged **leaves the training draw**: its layout weight drops to ``convergence.hold_share`` and
its adapter and head are frozen (animus.train, MappoTrainer.freeze_layouts), so the rest of the budget goes to the
classes still learning. It is still evaluated every evaluation, and if its score falls below its converged level by
more than the margin it re-enters at full weight and must converge again -- the trunk the other classes keep
training can move it. With every class converged the stage advances.

The controller only decides; animus.train carries it out.
"""

from __future__ import annotations

import math

from dataclasses import dataclass, field

import numpy as np

from .config import TrainConfig
from .evaluation import ConvergenceTracker
from .mappo.trainer import schedule

CONTINUE, ADVANCE = "continue", "advance"
SIGNALS = ("score", "kl", "entropy", "ladder")
RUNG_SETTLED = 0.5  # rungs a class's training difficulty may drift over the window
LEAGUE_SETTLED = 0.05  # win rate against the hardest league member, likewise


@dataclass
class Outcome:
    action: str  # CONTINUE or ADVANCE
    reason: str = ""  # finished.json reason: "converged" or "budget"
    report: dict | None = None  # per-class signals at the decision


@dataclass
class LayoutState:
    """One class's convergence signals, sampled at each evaluation."""

    name: str
    tracker: ConvergenceTracker
    kl: list[float] = field(default_factory=list)  # LR-normalised approx_kl, one mean per evaluation interval
    entropy: list[float] = field(default_factory=list)  # entropy / ln(allowed actions), likewise
    rung: list[float | None] = field(default_factory=list)  # mean training difficulty, likewise (None: no ladder)
    league: list[float | None] = field(default_factory=list)  # win rate vs the hardest member (None: no league)
    scores: list[float] = field(default_factory=list)
    converged: bool = False
    converged_score: float | None = None
    converged_margin: float = 0.0
    reentries: int = 0
    # Accumulators for the interval since the last evaluation.
    kl_sum: float = 0.0
    entropy_sum: float = 0.0
    updates: int = 0
    rung_sum: float = 0.0
    rung_count: int = 0
    league_latest: float | None = None
    played: bool = False  # had evaluation rows at some evaluation: a class the run never plays is not waited for

    def missing(self, config: TrainConfig, evals_needed: int) -> list[str]:
        """The signals this class has not satisfied over the window (empty = converged)."""
        c = config.convergence
        out = []
        if len(self.scores) < evals_needed or not self.tracker.converged(0, 0):
            out.append("score")
        window = self.kl[-evals_needed:]
        if len(window) < evals_needed or any(value > c.kl for value in window):
            out.append("kl")
        ratios = self.entropy[-evals_needed:]
        if len(ratios) < evals_needed:
            out.append("entropy")
        else:
            slope = abs(float(np.polyfit(np.arange(len(ratios)), np.array(ratios), 1)[0])) if len(ratios) > 1 else 0.0
            floor = config.entropy_floor.fraction
            if slope > c.entropy_slope or (floor > 0.0 and ratios[-1] < floor):
                out.append("entropy")
        rungs = [value for value in self.rung[-evals_needed:] if value is not None]
        leagues = [value for value in self.league[-evals_needed:] if value is not None]
        if len(self.rung) < evals_needed:
            out.append("ladder")
        elif rungs and max(rungs) - min(rungs) > RUNG_SETTLED:
            out.append("ladder")
        elif len(leagues) > 1 and max(leagues) - min(leagues) > LEAGUE_SETTLED:
            out.append("ladder")
        return out


class ConvergenceController:
    def __init__(self, config: TrainConfig, layout_names: list[str] | tuple[str, ...] = ()):
        self.config = config
        c = config.convergence
        self.evaluating = config.eval.every_env_steps > 0
        # The overall score's tracker decides best.pt and when the learning rate may start to anneal.
        self.tracker = self._tracker(c.patience if self.evaluating else 0)
        self.layouts: dict[str, LayoutState] = {
            name: LayoutState(name, self._tracker(c.window)) for name in layout_names}
        self.best_summary: dict | None = None
        self.baseline_summary: dict | None = None
        self.last_outcome: Outcome | None = None
        self.plateau_env_steps: int | None = None
        # What the entropy floor multiplies mappo.entropy_coef by; 1 until the floor has reason to raise it.
        self.entropy_scale = 1.0
        self.evals = 0

    def _tracker(self, patience: int) -> ConvergenceTracker:
        c = self.config.convergence
        return ConvergenceTracker(patience=patience, window=c.window, z=c.z, min_improvement=c.min_improvement,
                                  min_improvement_abs=c.min_improvement_abs)

    # ------------------------------------------------------------------ signals from training

    def observe_update(self, layout_stats: dict[str, dict], lr_scale: float) -> None:
        """Per-class statistics of one update (MappoTrainer.layout_stats joined with the buffer's allowed actions):
        {name: {"approx_kl": ..., "entropy": ..., "allowed_actions": ...}}."""
        for name, stats in layout_stats.items():
            state = self.layouts.get(name)
            if state is None:
                continue
            allowed = float(stats.get("allowed_actions", 0.0) or 0.0)
            state.kl_sum += float(stats.get("approx_kl", 0.0)) / max(lr_scale, 1e-6)
            state.entropy_sum += float(stats.get("entropy", 0.0)) / math.log(allowed) if allowed > 1.0 else 0.0
            state.updates += 1

    def observe_training_episodes(self, rungs: dict[str, float]) -> None:
        """Mean training difficulty per class over the episodes an update finished (a ladder stage)."""
        for name, rung in rungs.items():
            if (state := self.layouts.get(name)) is not None:
                state.rung_sum += float(rung)
                state.rung_count += 1

    def observe_league(self, hardest_win_rate: dict[str, float]) -> None:
        """The live policy's win rate against the hardest league member, per class (a league stage)."""
        for name, rate in hardest_win_rate.items():
            if (state := self.layouts.get(name)) is not None:
                state.league_latest = float(rate)

    def entropy_coef(self, env_steps: int) -> float:
        mappo = self.config.mappo
        return mappo.entropy_coef * self.entropy_scale * schedule(mappo.entropy_final_fraction, env_steps,
                                                                  self.config.total_env_steps)

    def lr_scale(self, env_steps: int) -> float:
        """The learning-rate factor: the configured linear anneal, held at 1 until the overall score has first
        plateaued (convergence.lr_hold_until_plateau) so the KL signal is not the schedule."""
        final = self.config.mappo.lr_final_fraction
        total = self.config.total_env_steps
        if not self.config.convergence.lr_hold_until_plateau or not self.evaluating:
            return schedule(final, env_steps, total)
        if self.plateau_env_steps is None:
            return 1.0
        return schedule(final, env_steps - self.plateau_env_steps, max(1, total - self.plateau_env_steps))

    def observe_entropy(self, entropy: float, allowed_actions: float) -> None:
        """Move the entropy floor after an update: `entropy` is the policy's, over `allowed_actions` legal ones.

        The ceiling a masked policy can reach is ln(allowed actions), so that -- not the padded action count --
        is what the target is a fraction of. The scale only ever sits between 1 and max_boost: below the target
        it climbs, above it falls back to the configured coefficient, so a policy that is converging on its own
        is never held open.
        """
        floor = self.config.entropy_floor
        if floor.fraction <= 0.0 or allowed_actions <= 1.0:
            return

        target = floor.fraction * math.log(allowed_actions)
        wanted = min(floor.max_boost, self.entropy_scale * 1.5) if entropy < target else 1.0
        self.entropy_scale += floor.rate * (wanted - self.entropy_scale)
        self.entropy_scale = min(max(self.entropy_scale, 1.0), max(1.0, floor.max_boost))

    # ------------------------------------------------------------------ evaluations

    def observe(self, summary: dict, env_steps: int) -> bool:
        """Record a learner evaluation; True if its networks are the new best (save them to best.pt)."""
        self.evals += 1
        stderr = summary.get("stderr", 0.0)
        improved = self.tracker.observe(summary["score"], env_steps, stderr)
        if improved:
            self.best_summary = summary
        if self.plateau_env_steps is None and self.tracker.converged(env_steps, 0):
            self.plateau_env_steps = env_steps

        rows = summary.get("layouts", {})
        for name, state in self.layouts.items():
            row = rows.get(name)
            interval_kl = state.kl_sum / state.updates if state.updates else 0.0
            interval_entropy = state.entropy_sum / state.updates if state.updates else 0.0
            rung = state.rung_sum / state.rung_count if state.rung_count else None
            state.kl_sum = state.entropy_sum = state.rung_sum = 0.0
            state.updates = state.rung_count = 0
            if row is None or row.get("score") is None:
                continue
            state.played = True
            score = float(row["score"])
            state.scores.append(score)
            state.tracker.observe(score, env_steps, float(row.get("stderr", 0.0) or 0.0))
            state.kl.append(interval_kl)
            state.entropy.append(interval_entropy)
            state.rung.append(rung)
            state.league.append(state.league_latest)

            if state.converged:
                # Re-entry: the trunk moved it back below where it converged.
                if state.converged_score is not None and score < state.converged_score - state.converged_margin:
                    state.converged = False
                    state.reentries += 1
                    state.tracker = self._tracker(self.config.convergence.window)
                    state.tracker.observe(score, env_steps, float(row.get("stderr", 0.0) or 0.0))
                    state.scores, state.kl, state.entropy = [score], [interval_kl], [interval_entropy]
                    state.rung, state.league = [rung], [state.league_latest]
            elif not state.missing(self.config, self.config.convergence.window):
                state.converged = True
                state.converged_score = score
                state.converged_margin = state.tracker.margin(float(row.get("stderr", 0.0) or 0.0))
        return improved

    def played_layouts(self) -> list[LayoutState]:
        return [state for state in self.layouts.values() if state.played]

    def converged_layouts(self) -> list[str]:
        return [state.name for state in self.played_layouts() if state.converged]

    def active_layouts(self) -> list[str]:
        return [state.name for state in self.played_layouts() if not state.converged]

    def weakest(self) -> tuple[str, list[str]] | None:
        """The unconverged class missing the most signals, and which."""
        window = self.config.convergence.window
        pending = [(state.name, state.missing(self.config, window)) for state in self.played_layouts()
                   if not state.converged]
        if not pending:
            return None
        return max(pending, key=lambda item: len(item[1]))

    def hold_weights(self) -> dict[str, float]:
        """Per class, the factor its training weight is multiplied by: hold_share once converged, else 1."""
        share = self.config.convergence.hold_share
        return {name: (share if state.converged else 1.0) for name, state in self.layouts.items()}

    def report(self) -> dict:
        window = self.config.convergence.window
        return {name: {
            "converged": state.converged,
            "reentries": state.reentries,
            "missing": state.missing(self.config, window) if state.played else ["never played"],
            "score": state.scores[-1] if state.scores else None,
            "kl": state.kl[-1] if state.kl else None,
            "entropy": state.entropy[-1] if state.entropy else None,
            "rung": state.rung[-1] if state.rung else None,
            "league": state.league[-1] if state.league else None,
        } for name, state in self.layouts.items()}

    def after_eval(self, env_steps: int) -> Outcome:
        """Call after each training evaluation: ADVANCE once every class the run plays has converged."""
        played = self.played_layouts()
        if played and self.evals >= self.config.convergence.window and all(s.converged for s in played):
            return self._decide(Outcome(ADVANCE, "converged", self.report()))
        return self._decide(Outcome(CONTINUE, report=self.report()))

    def at_budget(self) -> Outcome:
        """Call once total_env_steps is reached: the ceiling advances the stage, and the report says who was not
        done."""
        return self._decide(Outcome(ADVANCE, "budget", self.report()))

    def _decide(self, outcome: Outcome) -> Outcome:
        self.last_outcome = outcome
        return outcome

    # ------------------------------------------------------------------ persistence

    def state_dict(self) -> dict:
        """What a resumed run needs to carry on deciding (the overall tracker is saved on its own)."""
        return {
            "best_summary": self.best_summary,
            "baseline_summary": self.baseline_summary,
            "plateau_env_steps": self.plateau_env_steps,
            "evals": self.evals,
            "layouts": {name: {
                "tracker": state.tracker.state_dict(),
                "kl": state.kl, "entropy": state.entropy, "rung": state.rung, "league": state.league,
                "scores": state.scores, "converged": state.converged, "converged_score": state.converged_score,
                "converged_margin": state.converged_margin, "reentries": state.reentries, "played": state.played,
            } for name, state in self.layouts.items()},
        }

    def load_state_dict(self, state: dict | None) -> None:
        if not state:
            return
        self.best_summary = state.get("best_summary")
        self.baseline_summary = state.get("baseline_summary")
        self.plateau_env_steps = state.get("plateau_env_steps")
        self.evals = int(state.get("evals", 0))
        for name, saved in (state.get("layouts") or {}).items():
            layout = self.layouts.get(name)
            if layout is None:
                continue
            layout.tracker.load_state_dict(saved.get("tracker"))
            for key in ("kl", "entropy", "rung", "league", "scores"):
                setattr(layout, key, list(saved.get(key, [])))
            layout.converged = bool(saved.get("converged", False))
            layout.converged_score = saved.get("converged_score")
            layout.converged_margin = float(saved.get("converged_margin", 0.0))
            layout.reentries = int(saved.get("reentries", 0))
            layout.played = bool(saved.get("played", False))
