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
5. **At the top rung** (``convergence.top_rung``, every ladder stage) -- its training episodes have been at the top of
   their ladder (episode info ``at_top_rung``) for the whole window, and the score the plateau reads is its
   evaluation episodes' at the top rung only: a stage converges on its real task, never on an easier rung it has
   settled on. The score is the stage's own measure when ``convergence.measure`` names one (M1: ``arrived``).

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
SIGNALS = ("score", "kl", "entropy", "ladder", "top_rung")
TOP_RUNG_SHARE = 0.9  # of a class's training episodes at the top rung, every evaluation interval of the window
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
    top: list[float | None] = field(default_factory=list)  # share of training episodes at the top rung (None: none)
    ladder: bool = False  # a ladder stage's class (it has a top rung): convergence waits for it
    top_scored: bool = False  # the latest evaluation scored it at the top rung
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
    top_sum: float = 0.0
    top_count: int = 0
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
        if c.top_rung and self.ladder:
            # The training share is read where the sim reports it (at_top_rung); a stage that reports only its
            # difficulty tiers is judged on the evaluation's top tier alone.
            tops = self.top[-evals_needed:]
            reported = any(value is not None for value in self.top)
            if not self.top_scored or (reported and (len(tops) < evals_needed or any(
                    value is None or value < TOP_RUNG_SHARE for value in tops))):
                out.append("top_rung")
        return out


class ShapingFade:
    """The shaping ladder (FadeConfig): which rung the stage is on, and when it moves.

    Each rung has its own tracker of the outcome score, so a plateau is read against the rung's own best: carried over,
    a best from the rung above would make the dip a step costs read as the plateau that allows the next step."""

    NAME = "shaping ladder"
    FORWARD = "steps down"   # a rung further from the first
    BACK = "steps back up"

    def __init__(self, config: TrainConfig, ladder=None):
        fade = config.fade if ladder is None else ladder
        self.enabled = bool(fade.enabled) and len(fade.rungs) > 0
        self.gate_metric = str(getattr(fade, "gate_metric", "") or "")
        self.gate_value = float(getattr(fade, "gate_value", 0.0))
        self.gate_seen: float | None = None
        self.rungs = tuple(float(scale) for scale in fade.rungs) or (1.0,)
        self.window = max(1, int(fade.window))
        self.regress_z = float(fade.regress_z)
        self.give_up = max(1, int(fade.give_up))
        self._convergence = config.convergence
        self.rung = 0
        self.tracker = self._new_tracker()
        self.evals_at_rung = 0
        # The score and its standard error at the last step down, which a regression is measured from.
        self.step_score: float | None = None
        self.step_stderr = 0.0
        self.falls: dict[int, int] = {}  # rung -> times the ladder fell back to it
        self.steps = 0

    def _new_tracker(self) -> ConvergenceTracker:
        c = self._convergence
        return ConvergenceTracker(patience=self.window, window=self.window, z=c.z, min_improvement=c.min_improvement,
                                  min_improvement_abs=c.min_improvement_abs)

    @property
    def scale(self) -> float:
        return self.rungs[self.rung] if self.enabled else 1.0

    @property
    def held(self) -> bool:
        """Fallen back to this rung often enough that the ladder stays."""
        return self.falls.get(self.rung, 0) >= self.give_up

    @property
    def settled(self) -> bool:
        """The convergence signal: at the last rung, or held. Off, there is nothing to wait for."""
        return not self.enabled or self.rung == len(self.rungs) - 1 or self.held

    def observe(self, score: float, stderr: float, env_steps: int, ladders_settled: bool = True,
                anneal_starting: bool = False) -> str | None:
        """Record an evaluation's outcome score; the log line when the ladder moved, else None.

        It never steps down while a ladder stage's difficulty is still moving (`ladders_settled`), nor at the
        evaluation where the learning-rate anneal begins (`anneal_starting`): two changes at once cannot be told
        apart in the score that judges them. A regression steps back up whatever else is moving."""
        if not self.enabled:
            return None
        self.tracker.observe(score, env_steps, stderr)
        self.evals_at_rung += 1
        waited = self.evals_at_rung

        # A regression is read off the rung's last `window` scores, once it has that many: one noisy evaluation at
        # regress_z 2 is a ~2% false alarm, which over a rung of dozens of evaluations would hold the ladder short of
        # the outcome alone after give_up falls; and the first evaluations after a step carry the dip the step
        # causes, which the policy has not yet had time to adapt to.
        if self.step_score is not None and self.rung > 0 and waited >= self.window:
            recent = self.tracker.history[-self.window:]
            mean = sum(point[1] for point in recent) / len(recent)
            mean_stderr = sum(point[2] ** 2 for point in recent) ** 0.5 / len(recent)
            noise = self.regress_z * (mean_stderr ** 2 + self.step_stderr ** 2) ** 0.5
            if mean < self.step_score - noise:
                score, stderr = mean, mean_stderr
                before, reference, reference_stderr = self.scale, self.step_score, self.step_stderr
                self.rung -= 1
                self.falls[self.rung] = self.falls.get(self.rung, 0) + 1
                self._moved()
                self.step_score = None
                return (f"the {self.NAME} {self.BACK}, x{before:g} -> x{self.scale:g}: outcome score "
                        f"{score:.4g} +/- {stderr:.2g} over the last {len(recent)} evaluations against "
                        f"{reference:.4g} +/- {reference_stderr:.2g} at the step "
                        f"(more than {self.regress_z:g} standard errors below) after {waited} evaluations"
                        + (f"; held here after {self.falls[self.rung]} falls" if self.held else ""))

        if (self.rung < len(self.rungs) - 1 and not self.held and ladders_settled and not anneal_starting
                and self._earned(env_steps, waited)):
            before, reference = self.scale, self.step_score
            self.rung += 1
            self.step_score, self.step_stderr = score, stderr
            self._moved()
            against = f" (the last step's {reference:.4g})" if reference is not None else ""
            return (f"the {self.NAME} {self.FORWARD}, x{before:g} -> x{self.scale:g}: outcome score {score:.4g} +/- "
                    f"{stderr:.2g} {self._why()}{against} after {waited} evaluations")
        return None

    def see_gate(self, summary: dict) -> None:
        """The latest evaluation's gate metric (the ladder config's gate_metric), read before observe()."""
        value = summary.get(self.gate_metric) if self.gate_metric else None
        self.gate_seen = float(value) if isinstance(value, (int, float)) else None

    def _gated(self) -> bool:
        """The gate metric has reached its value, or there is no gate."""
        return not self.gate_metric or (self.gate_seen is not None and self.gate_seen >= self.gate_value)

    def _plateaued(self, env_steps: int, waited: int) -> bool:
        """The rung has been played long enough and the score has plateaued on it."""
        return waited >= self.window and self.tracker.converged(env_steps, 0)

    def _earned(self, env_steps: int, waited: int) -> bool:
        """Plateaued, and the stage's own measure there (the gate) when the ladder has one."""
        return self._plateaued(env_steps, waited) and self._gated()

    def _moved(self) -> None:
        self.steps += 1
        self.evals_at_rung = 0
        self.tracker = self._new_tracker()

    def _why(self) -> str:
        if self.gate_metric and self.gate_seen is not None:
            return f"plateaued with {self.gate_metric} {self.gate_seen:.3g} (gate {self.gate_value:g})"
        return "plateaued"

    def forget_scores(self) -> None:
        """Scores of another kind are coming (restore_evaluation_state): keep the rung, drop what was measured."""
        self.tracker = self._new_tracker()
        self.evals_at_rung = 0
        self.step_score = None
        self.step_stderr = 0.0

    def report(self) -> dict:
        return {"scale": self.scale, "rung": self.rung, "settled": self.settled, "steps": self.steps}

    def state_dict(self) -> dict:
        return {"rung": self.rung, "tracker": self.tracker.state_dict(), "evals_at_rung": self.evals_at_rung,
                "step_score": self.step_score, "step_stderr": self.step_stderr,
                "falls": {str(rung): count for rung, count in self.falls.items()}, "steps": self.steps}

    def load_state_dict(self, state: dict | None) -> None:
        if not state:
            return
        self.rung = min(max(0, int(state.get("rung", 0))), len(self.rungs) - 1)
        self.tracker.load_state_dict(state.get("tracker"))
        self.evals_at_rung = int(state.get("evals_at_rung", 0))
        self.step_score = state.get("step_score")
        self.step_stderr = float(state.get("step_stderr", 0.0))
        self.falls = {int(rung): int(count) for rung, count in (state.get("falls") or {}).items()}
        self.steps = int(state.get("steps", 0))


class CostLadder(ShapingFade):
    """The cost ladder (CostLadderConfig): the shaping ladder's rules on the noise prices, along any path of rungs.

    It reads the same outcome score, which is at full price whatever the rung, so a step costs the score nothing
    unless the policy gives up the outcome for it, or its habits for the lower price -- and that is the regression
    that moves it back. Off the first rung it waits for the gate metric instead of a plateau (see_gate)."""

    NAME = "cost ladder"
    FORWARD = "moves on"
    BACK = "moves back"

    def __init__(self, config: TrainConfig):
        super().__init__(config, config.costs)
        self.reshaped = False

    def load_state_dict(self, state: dict | None) -> None:
        """As the shaping ladder's; a checkpoint whose rung is past the configured ladder's end (the rungs were cut
        short since) lands on the last rung as a fresh step: what it played there was another price, so the wait,
        the plateau and the step score start over (`reshaped`, for the controller to start its own over)."""
        self.reshaped = False
        super().load_state_dict(state)
        if state and int(state.get("rung", 0)) > len(self.rungs) - 1:
            self.reshaped = True
            self.evals_at_rung = 0
            self.tracker = self._new_tracker()
            self.step_score = None
            self.step_stderr = 0.0

    def _earned(self, env_steps: int, waited: int) -> bool:
        # The gate replaces the plateau off the first rung only; later rungs step on a plateau alone.
        if self.rung == 0 and self.gate_metric:
            return self._gated()
        return self._plateaued(env_steps, waited)

    def _why(self) -> str:
        if self.rung == 1 and self.gate_metric and self.gate_seen is not None:
            return f"({self.gate_metric} {self.gate_seen:.3g} reached {self.gate_value:g})"
        return "plateaued"

    @property
    def ready(self) -> bool:
        """Settled and played at that rung for `window` evaluations (or off): what convergence and the learning-rate
        anneal wait for, so a policy is judged and annealed only once it has met the full price."""
        return not self.enabled or (self.settled and self.evals_at_rung >= self.window)


def restore_evaluation_state(tracker: ConvergenceTracker, controller: "ConvergenceController", checkpoint: dict,
                             score_kind: str) -> str | None:
    """Load a resumed checkpoint's evaluation state, unless its scores are of another kind than the run's now.

    `score_kind` is the episode info column the run scores on ("" = the return; EvalResult.score_column). A checkpoint
    from before the outcome score (peak-play W0) carries none and was scored on the return. When the kinds differ the
    tracker and the controller's score-based state start over -- best.pt stays on disk, but the next evaluation is
    the new best -- and the reason is returned for the run to print; None when nothing had to be dropped.
    """
    tracker.load_state_dict(checkpoint.get("convergence"))
    controller.load_state_dict(checkpoint.get("controller"))
    saved = checkpoint.get("score_kind", "")
    if saved == score_kind:
        return None
    tracker.forget_scores()
    controller.forget_scores()
    return (f"the checkpoint's evaluations were scored on {saved or 'the return'} and this run scores on "
            f"{score_kind or 'the return'}: the best score and the convergence history start over (best.pt is kept)")


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
        # The shaping ladder (FadeConfig), and what it said at the latest evaluation for the run to print.
        self.fade = ShapingFade(config)
        self.fade_message: str | None = None
        # The cost ladder (CostLadderConfig), likewise.
        self.costs = CostLadder(config)
        self.costs_message: str | None = None

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

    def observe_training_episodes(self, rungs: dict[str, float], tops: dict[str, float] | None = None) -> None:
        """Mean training difficulty per class over the episodes an update finished (a ladder stage), and the share of
        them at the top rung (episode info at_top_rung, when the sim reports it)."""
        for name, rung in rungs.items():
            if (state := self.layouts.get(name)) is not None:
                state.rung_sum += float(rung)
                state.rung_count += 1
        for name, share in (tops or {}).items():
            if (state := self.layouts.get(name)) is not None:
                state.top_sum += float(share)
                state.top_count += 1
                state.ladder = True

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
        # The cost ladder climbs first; the anneal waits until it has been at full price for its window.
        costs_ready = self.costs.ready
        if self.plateau_env_steps is None and costs_ready and self.tracker.converged(env_steps, 0):
            self.plateau_env_steps = env_steps
        # Read before this evaluation's rungs join the classes' lists below: the ladder as it stood over the window.
        anneal_starting = self.plateau_env_steps == env_steps
        ladders_settled = self.ladders_settled()
        self.costs.see_gate(summary)
        self.fade.see_gate(summary)
        self.costs_message = self.costs.observe(summary["score"], stderr, env_steps, ladders_settled,
                                                anneal_starting)
        # The shaping ladder waits for the cost ladder: two scales moving at once cannot be told apart in the score.
        self.fade_message = self.fade.observe(summary["score"], stderr, env_steps,
                                              ladders_settled and costs_ready and self.costs_message is None,
                                              anneal_starting)

        rows = summary.get("layouts", {})
        if not rows and len(self.layouts) == 1:
            rows = {next(iter(self.layouts)): summary}      # one class: the summary is its row
        top = summary.get("top_rung") if self.config.convergence.top_rung else None
        top_rows = (top.get("layouts") or ({next(iter(self.layouts)): top} if len(self.layouts) == 1 else {})
                    ) if top is not None else {}
        for name, state in self.layouts.items():
            row = rows.get(name)
            interval_kl = state.kl_sum / state.updates if state.updates else 0.0
            interval_entropy = state.entropy_sum / state.updates if state.updates else 0.0
            rung = state.rung_sum / state.rung_count if state.rung_count else None
            interval_top = state.top_sum / state.top_count if state.top_count else None
            state.kl_sum = state.entropy_sum = state.rung_sum = state.top_sum = 0.0
            state.updates = state.rung_count = state.top_count = 0
            if row is None or row.get("score") is None:
                continue
            state.played = True
            if top is not None:
                state.ladder = True
            state.kl.append(interval_kl)
            state.entropy.append(interval_entropy)
            state.rung.append(rung)
            state.league.append(state.league_latest)
            state.top.append(interval_top)
            # A ladder stage's class is judged at the top rung only (convergence.top_rung), on the stage's own measure;
            # an evaluation with none of its episodes there adds no score (it cannot converge on another rung's).
            judged = top_rows.get(name) if state.ladder and top is not None else row
            state.top_scored = judged is not None and judged.get("score") is not None
            if not state.top_scored:
                continue
            score, stderr = self._judged_score(judged)
            state.scores.append(score)
            state.tracker.observe(score, env_steps, stderr)

            if state.converged:
                # Re-entry: the trunk moved it back below where it converged.
                if state.converged_score is not None and score < state.converged_score - state.converged_margin:
                    state.converged = False
                    state.reentries += 1
                    state.tracker = self._tracker(self.config.convergence.window)
                    state.tracker.observe(score, env_steps, stderr)
                    state.scores, state.kl, state.entropy = [score], [interval_kl], [interval_entropy]
                    state.rung, state.league, state.top = [rung], [state.league_latest], [interval_top]
            elif costs_ready and not state.missing(self.config, self.config.convergence.window):
                state.converged = True
                state.converged_score = score
                state.converged_margin = state.tracker.margin(stderr)
        return improved

    def _judged_score(self, row: dict) -> tuple[float, float]:
        """A class's convergence score from its evaluation row: the stage's own measure when convergence.measure
        names a column the row has (a share's standard error from its episodes), else the score."""
        measure = self.config.convergence.measure
        if measure and row.get(measure) is not None:
            value = float(row[measure])
            episodes = max(1, int(row.get("episodes", 1) or 1))
            share = min(1.0, max(0.0, value))
            stderr = math.sqrt(max(share * (1.0 - share), 1e-4) / episodes) if 0.0 <= value <= 1.0 else float(
                row.get("stderr", 0.0) or 0.0)
            return value, stderr
        return float(row["score"]), float(row.get("stderr", 0.0) or 0.0)

    def ladders_settled(self) -> bool:
        """The played classes' difficulty ladders (a ladder stage's rungs) have settled over the window, all but at most
        fade.moving_classes of them; True without one. The shaping ladder waits for it: two ladders moving at once
        cannot be told apart in the score. A few classes still moving do not move the score much; waiting for every
        one of ten never ended (stage4, 2026-10-03)."""
        window = self.config.convergence.window
        moving = 0
        for state in self.played_layouts():
            rungs = [value for value in state.rung[-window:] if value is not None]
            if rungs and max(rungs) - min(rungs) > RUNG_SETTLED:
                moving += 1
        return moving <= self.config.fade.moving_classes

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
            "top_rung": state.top[-1] if state.top else None,
        } for name, state in self.layouts.items()}

    def after_eval(self, env_steps: int) -> Outcome:
        """Call after each training evaluation: ADVANCE once every class the run plays has converged."""
        played = self.played_layouts()
        if (self.config.convergence.advance and played and self.evals >= self.config.convergence.window
                and all(s.converged for s in played) and self.fade.settled and self.costs.ready):
            return self._decide(Outcome(ADVANCE, "converged", self.report()))
        return self._decide(Outcome(CONTINUE, report=self.report()))

    def at_budget(self) -> Outcome:
        """Call once total_env_steps is reached: the ceiling advances the stage, and the report says who was not
        done."""
        return self._decide(Outcome(ADVANCE, "budget", self.report()))

    def _decide(self, outcome: Outcome) -> Outcome:
        self.last_outcome = outcome
        return outcome

    def forget_scores(self) -> None:
        """Drop the score-based state -- the best summary, each class's tracker, scores and convergence -- when the
        scores to come are of another kind (restore_evaluation_state). The signals with no score in them (KL, entropy,
        rung, league, the plateau the learning rate anneals from) are kept: they mean the same either way."""
        self.best_summary = None
        self.baseline_summary = None
        self.fade.forget_scores()
        self.costs.forget_scores()
        for state in self.layouts.values():
            state.tracker = self._tracker(self.config.convergence.window)
            state.scores = []
            state.converged = False
            state.converged_score = None
            state.converged_margin = 0.0

    # ------------------------------------------------------------------ persistence

    def state_dict(self) -> dict:
        """What a resumed run needs to carry on deciding (the overall tracker is saved on its own)."""
        return {
            "best_summary": self.best_summary,
            "baseline_summary": self.baseline_summary,
            "plateau_env_steps": self.plateau_env_steps,
            "evals": self.evals,
            "fade": self.fade.state_dict(),
            "costs": self.costs.state_dict(),
            "layouts": {name: {
                "tracker": state.tracker.state_dict(),
                "kl": state.kl, "entropy": state.entropy, "rung": state.rung, "league": state.league,
                "top": state.top, "ladder": state.ladder, "top_scored": state.top_scored,
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
        self.fade.load_state_dict(state.get("fade"))
        self.costs.load_state_dict(state.get("costs"))
        for name, saved in (state.get("layouts") or {}).items():
            layout = self.layouts.get(name)
            if layout is None:
                continue
            layout.tracker.load_state_dict(saved.get("tracker"))
            for key in ("kl", "entropy", "rung", "league", "top", "scores"):
                setattr(layout, key, list(saved.get(key, [])))
            layout.ladder = bool(saved.get("ladder", False))
            layout.top_scored = bool(saved.get("top_scored", False))
            layout.converged = bool(saved.get("converged", False))
            layout.converged_score = saved.get("converged_score")
            layout.converged_margin = float(saved.get("converged_margin", 0.0))
            layout.reentries = int(saved.get("reentries", 0))
            layout.played = bool(saved.get("played", False))
        if self.costs.reshaped:
            # The ladder was cut short under a resumed run (stage1_move's fade-out, 2026-10-04: the habits did not
            # stick without the price): the classes converged at another price, and the learning rate annealed from
            # a plateau at it. Both start over at the ladder's last rung.
            self.plateau_env_steps = None
            for layout in self.layouts.values():
                layout.converged = False
                layout.converged_score = None
                layout.converged_margin = 0.0
                layout.tracker = self._tracker(self.config.convergence.window)
                layout.scores = []
