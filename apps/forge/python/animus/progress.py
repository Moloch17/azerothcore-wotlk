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

PROGRESS_FILE = "progress.json"


def _clean(value):
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, (int, str)) or value is None:
        return value
    return float(value)


def write_progress(run_dir: Path, fields: dict) -> Path:
    """Atomically write ``fields`` (flat: numbers, strings, None) to ``run_dir/progress.json``."""
    row: dict = {}
    nonfinite = []
    for key, value in fields.items():
        value = _clean(value)
        if isinstance(value, float) and not math.isfinite(value):
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

    def __init__(self, run_dir: Path, config, spec, resumed_update: int = 0, resumed_env_steps: int = 0):
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
        }
        self.metrics: dict = {}
        self.evaluation: dict = {}

    def training(self, row: dict) -> None:
        """An update's metrics row (train.py's metrics.csv row)."""
        self.metrics = dict(row)

    def evaluated(self, env_steps: int, score: float, baseline_score: float | None, tracker, controller=None) -> None:
        weakest = controller.weakest() if controller else None
        self.evaluation = {
            "evals": len(tracker.history),
            "last_eval_env_steps": env_steps,
            "last_eval_score": score,
            "baseline_score": baseline_score,
            "best_score": tracker.best,
            "best_env_steps": tracker.best_env_steps,
            "evals_since_best": tracker.evals_since_best,
            # The convergence rule per class (animus.stage): who is done, who is not, and what the one furthest
            # from done is still missing.
            "converged_layouts": ",".join(controller.converged_layouts()) if controller else "",
            "active_layouts": ",".join(controller.active_layouts()) if controller else "",
            "weakest_layout": weakest[0] if weakest else "",
            "weakest_missing": ",".join(weakest[1]) if weakest else "",
            "reentries": sum(state.reentries for state in controller.layouts.values()) if controller else 0,
        }

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
        }
        return write_progress(self.run_dir, fields)
