"""Live progress of a training run, for the sim's console.

The learner rewrites ``runs/<run>/progress.json`` after every update and around every evaluation. The worldserver
reads it for ``forge status`` and its periodic progress report (steps, ETA, evaluation scores, health warnings).

The file is one flat JSON object -- numbers, strings and nulls, no nesting: the sim reads only top-level values
(src/Console/Progress.cpp, with Boost.JSON). A metric that is NaN or infinite is written as null and named in
``nonfinite``. The file is written to a temporary name and renamed, so a reader never sees half of it.
"""

from __future__ import annotations

import json
import math
import time
from pathlib import Path

from .evaluation import ABLATION_COLUMNS

PROGRESS_FILE = "progress.json"

#: Split measures only an evaluation arm reads (H, dungeon-curriculum I7): arm -> {its column: (the plain evaluation's
#: column it stands beside, the gap's name)}. The plain evaluation is all bots, so the stand-in party's clears are the
#: with_human arm's; the gap is all bots minus the stand-in party.
ARM_SPLITS = {"with_human": {"clear_standin": ("clear_allbot", "standin_gap")}}


def _clean(value):
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, (int, str)) or value is None:
        return value
    return float(value)


def write_progress(run_dir: Path, fields: dict, undefined: frozenset[str] | set[str] = frozenset()) -> Path:
    """Atomically write ``fields`` (flat: numbers, strings, None) to ``run_dir/progress.json``.

    A key in ``undefined`` that is NaN is written as null without being named in ``nonfinite``: a per-event mean with no
    event to average (a rung's found rate before the ladder reaches it), not a fault."""
    row: dict = {}
    nonfinite = []
    for key, value in fields.items():
        value = _clean(value)
        if isinstance(value, float) and not math.isfinite(value):
            if not (key in undefined and math.isnan(value)):
                nonfinite.append(key)
            value = None
        row[key] = value
    row["nonfinite"] = ",".join(nonfinite)

    path = run_dir / PROGRESS_FILE
    partial = path.with_suffix(path.suffix + ".partial")
    partial.write_text(json.dumps(row, indent=1) + "\n")
    partial.replace(path)
    return path


class ProgressWriter:
    """Keeps the latest training metrics and evaluation between writes, so every write is a complete picture."""

    def __init__(self, run_dir: Path, config, spec, resumed_update: int = 0, resumed_env_steps: int = 0,
                 stage: dict | None = None):
        self.run_dir = run_dir
        self.static = {
            "run_name": config.run_name,
            "scenario": spec.scenario,
            "total_env_steps": config.total_env_steps,
            "started_at": time.time(),
            "resumed_update": resumed_update,
            "resumed_env_steps": resumed_env_steps,
            "eval_every": config.eval.every_env_steps,
            "patience": config.convergence.patience if config.eval.every_env_steps > 0 else 0,
            "window": config.convergence.window,
            "baseline": config.eval.baseline,
            # The stage's own measures for forge status (config.status): their order, and "metric>=x;metric<=y".
            "status_headline": ",".join(config.status.headline),
            "status_targets": config.status.target_text(),
            # The classes it never fields, by design, and why: "death_knight=...;...".
            "status_excluded": config.status.excluded_text(),
        }
        self.headline = tuple(config.status.headline)
        self.metrics: dict = {}
        self.undefined: frozenset[str] = frozenset()
        self.evaluation: dict = {}
        # The evaluation arms' readings (eval.arms), kept apart: an arm plays every eval.arms_every evaluations, and
        # its last reading stands until the next.
        self.arms: dict = {}
        # The held-out arenas' readings (eval.heldout; general search sec 7), kept apart likewise: an arena plays on
        # its cadence, and its last reading stands until the next.
        self.heldout: dict = {}
        # Each arena's map (stage.json arenas[].map_id, absent in a manifest before general search) and the maps the
        # stage trains on (its arenas that are not eval_only): a held-out arena on a map the stage never trains on
        # measures transfer, and its `found` feeds eval_found_heldout_map.
        arenas = list((stage or {}).get("arenas", ()))
        self.arena_maps = {str(arena.get("name")): arena.get("map_id") for arena in arenas}
        self.trained_maps = {arena.get("map_id") for arena in arenas
                             if not arena.get("eval_only") and arena.get("map_id") is not None}
        # arena -> (found, episodes) of the held-out arenas on an untrained map, as last played.
        self.heldout_found: dict = {}

    def arm_columns(self, arm: str) -> tuple[str, ...]:
        """The episode columns an arm's summary needs for the headline: clear_rate for clear_rate_with_human."""
        suffix = f"_{arm}"
        # arrived_no_compass is a plain column that ends in the no_compass arm's suffix, not that arm's arrived.
        return tuple(metric[:-len(suffix)] for metric in self.headline
                     if metric.endswith(suffix) and metric not in ABLATION_COLUMNS)

    def arm_evaluated(self, arm: str, summary: dict) -> None:
        """An evaluation arm's summary: its score as eval_<arm>_score, and each headline metric <metric>_<arm> as
        eval_<metric>_<arm>, so forge status shows it beside the plain (all bots) reading."""
        self.arms[f"eval_{arm}_score"] = summary.get("score")
        self.arms[f"eval_{arm}_episodes"] = summary.get("episodes")
        for metric in self.arm_columns(arm):
            self.arms[f"eval_{metric}_{arm}"] = summary.get(metric)
        # A split measure only the arm reads (ARM_SPLITS: clear_standin -- the plain evaluation is all bots, so it has
        # none) is the arm's eval_<metric>, and its gap to the plain evaluation's all-bot reading eval_<gap> (H: the
        # stand-in party within ~10 points of the all-bot one).
        for metric, (plain, gap) in ARM_SPLITS.get(arm, {}).items():
            if metric not in self.headline and gap not in self.headline:
                continue
            value = summary.get(metric)
            self.arms[f"eval_{metric}"] = value
            base = self.evaluation.get(f"eval_{plain}")
            self.arms[f"eval_{gap}"] = (float(base) - float(value)
                                        if isinstance(base, (int, float)) and isinstance(value, (int, float))
                                        else None)

    def heldout_columns(self, name: str) -> tuple[str, ...]:
        """The episode columns a held-out arena's summary needs for the headline: found for found_heldout_deadmines."""
        suffix = f"_heldout_{name}"
        return tuple(metric[:-len(suffix)] for metric in self.headline if metric.endswith(suffix))

    def heldout_evaluated(self, name: str, summary: dict, map_id=None) -> None:
        """A held-out arena's summary (general search sec 7): each headline metric <metric>_heldout_<name> as
        eval_<metric>_heldout_<name> (the arm pattern), its episodes as eval_heldout_<name>_episodes, and
        eval_found_heldout_map: the `found` of the held-out arenas whose map (`map_id`, the manifest's unless given) is
        not any trainable arena's, averaged over their episodes -- the transfer headline (M2's Deadmines; the Stockades
        sweeps are on the training map and never count). Absent while no such arena has played, or when the manifest
        carries no map ids."""
        self.heldout[f"eval_heldout_{name}_episodes"] = summary.get("episodes")
        for metric in self.heldout_columns(name):
            self.heldout[f"eval_{metric}_heldout_{name}"] = summary.get(metric)
        if map_id is None:
            map_id = self.arena_maps.get(name)
        if map_id is not None and map_id not in self.trained_maps:
            found, episodes = summary.get("found"), summary.get("episodes")
            if isinstance(found, (int, float)) and isinstance(episodes, (int, float)) and episodes > 0:
                self.heldout_found[name] = (float(found), float(episodes))
        if self.heldout_found:
            total = sum(episodes for _, episodes in self.heldout_found.values())
            self.heldout["eval_found_heldout_map"] = (
                sum(found * episodes for found, episodes in self.heldout_found.values()) / total if total > 0 else None)

    def note(self, key: str, text: str) -> None:
        """A line of state forge status shows as it is (stand_in: whether the "human" stand-in is fielded, and why
        not), kept until it changes."""
        self.static[key] = text

    def training(self, row: dict, undefined: frozenset[str] | set[str] = frozenset()) -> None:
        """An update's metrics row (train.py's metrics.csv row); `undefined` names its NaN columns that are by design
        (episode_means.undefined) and so not warned about."""
        self.metrics = dict(row)
        self.undefined = frozenset(undefined)

    def evaluated(self, env_steps: int, score: float, baseline_score: float | None, tracker, controller=None,
                  summary: dict | None = None) -> None:
        weakest = controller.weakest() if controller else None
        self.evaluation = {
            "evals": len(tracker.history),
            "last_eval_env_steps": env_steps,
            "last_eval_score": score,
            "baseline_score": baseline_score,
            # What best_score is of: the stage's measure (a share) or, "score", the evaluation score.
            "best_score": tracker.best,
            "best_kind": (controller.measure if controller else "") or "score",
            "best_env_steps": tracker.best_env_steps,
            "evals_since_best": tracker.evals_since_best,
            # The convergence rule per class (animus.stage): who is done, who is not, and what the one furthest
            # from done is still missing.
            "converged_layouts": ",".join(controller.converged_layouts()) if controller else "",
            "active_layouts": ",".join(controller.active_layouts()) if controller else "",
            "weakest_layout": weakest[0] if weakest else "",
            "weakest_missing": ",".join(weakest[1]) if weakest else "",
            "reentries": sum(state.reentries for state in controller.layouts.values()) if controller else 0,
            # The headline measures' evaluation means (eval_<metric>), beside the training means (episode_<metric>);
            # the arms' and the held-out arenas' readings are theirs (kept apart, written last).
            **{f"eval_{metric}": self._headline_value(summary or {}, metric) for metric in self.headline
               if f"eval_{metric}" not in self.arms and f"eval_{metric}" not in self.heldout},
        }

    @staticmethod
    def _headline_value(summary: dict, metric: str):
        """A headline metric's reading: the summary's own column, else -- for a metric named <metric>_arena_<arena>
        that is not a column of its own (find_seconds_arena_ragefire; the sim's found_arena_<arena> columns are) -- that
        arena's <metric> from the summary's per-arena split (general search sec 7)."""
        value = summary.get(metric)
        if value is None and "_arena_" in metric:
            base, arena = metric.split("_arena_", 1)
            value = ((summary.get("arenas") or {}).get(arena) or {}).get(base)
        return value

    def restore_evaluation(self, tracker, baseline_score: float | None, controller=None) -> None:
        """After a resume: the evaluation state the checkpoint carried."""
        if tracker.history:
            env_steps, score = tracker.history[-1][:2]
            self.evaluated(env_steps, score, baseline_score, tracker, controller)

    def write(self, phase: str, update: int, env_steps: int, finish_reason: str = "", advanced: bool = False) -> Path:
        fields = {
            **self.static,
            "phase": phase,
            "update": update,
            "env_steps": env_steps,
            "updated_at": time.time(),
            "finish_reason": finish_reason,
            "advanced": advanced,
            **{k: v for k, v in self.metrics.items() if k not in ("update", "env_steps")},
            **self.evaluation,
            **self.arms,
            **self.heldout,
        }
        return write_progress(self.run_dir, fields, self.undefined)
