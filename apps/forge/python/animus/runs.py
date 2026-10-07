"""Run directories.

A learner trains from scratch by default. When it starts, whatever its run directory holds from an earlier run is
moved to ``<output_dir>/archive/<run>-<time>/`` (nothing is deleted): beside ``runs/`` rather than inside it, so
TensorBoard, which reads ``runs/``, loads the live runs and not every run ever archived. While it trains, only the newest numbered
checkpoints are kept (latest.pt and best.pt are separate files and always stay).

``--resume`` (the sim's ``forge resume``) is the one exception: it continues the run in place from its
``latest.pt``, provided the scenario still has the same shapes (see ``resume_mismatch``).
"""

from __future__ import annotations

import shutil
import time
from pathlib import Path

ARCHIVE_DIR = "archive"           # a sibling of runs/ (<OutputDir>/archive); runs/_archive before 2026-10-03
CHECKPOINT_GLOB = "checkpoint_*.pt"
LATEST_CHECKPOINT = "latest.pt"
FINISHED_FILE = "finished.json"

# The spec fields the networks' shapes depend on. The env count, decision interval and episode length may change
# between a run and its resume.
RESUME_SPEC_KEYS = ("scenario", "agents_per_env", "obs_dim", "state_dim", "num_actions", "layouts")


def prune_checkpoints(run_dir: Path, keep: int) -> list[Path]:
    """Delete all but the newest `keep` numbered checkpoints (0 keeps them all); returns the deleted files."""
    if keep <= 0:
        return []

    # checkpoint_<update, zero-padded>.pt: name order is update order.
    checkpoints = sorted(run_dir.glob(CHECKPOINT_GLOB))
    removed = checkpoints[:-keep]
    for path in removed:
        path.unlink(missing_ok=True)
    return removed


def rung_best_name(ladder: str, rung: int) -> str:
    """The file a gate-stepped ladder's easier rung keeps its best in: best_rung<k>.pt for the shaping ladder (the
    fade), best_<ladder>_rung<k>.pt for another. Outside CHECKPOINT_GLOB, so the rotation never prunes it."""
    return f"best_rung{rung}.pt" if ladder == "fade" else f"best_{ladder}_rung{rung}.pt"


def archive_rung_best(run_dir: Path, ladder: str, rung: int, best: Path | None = None) -> Path | None:
    """Copy `best` (the run's best.pt, which is the best of the rung being left) to its rung's own file; the path, or
    None when there was nothing to copy (no best.pt yet, as on a run resumed without one). Written then renamed, so a
    stop mid-copy never leaves a truncated archive, and an archive already there is replaced."""
    best = best if best is not None else run_dir / "best.pt"
    if not best.is_file():
        return None
    target = run_dir / rung_best_name(ladder, rung)
    partial = target.with_suffix(target.suffix + ".partial")
    shutil.copyfile(best, partial)
    partial.replace(target)
    return target


def archive_run(run_dir: Path) -> Path | None:
    """Move an earlier run out of ``run_dir`` and leave it empty; returns where it went, if there was one."""
    archived = None
    if run_dir.exists() and any(run_dir.iterdir()):
        archive = run_dir.parent.parent / ARCHIVE_DIR
        archive.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        archived = archive / f"{run_dir.name}-{stamp}"
        suffix = 1
        while archived.exists():
            archived = archive / f"{run_dir.name}-{stamp}-{suffix}"
            suffix += 1
        run_dir.rename(archived)

    run_dir.mkdir(parents=True, exist_ok=True)
    return archived


def resume_checkpoint_path(run_dir: Path) -> Path:
    """The checkpoint a resume continues from; raises FileNotFoundError when the run has none."""
    path = run_dir / LATEST_CHECKPOINT
    if not path.exists():
        raise FileNotFoundError(f"cannot resume {run_dir.name}: {path} does not exist")
    return path


def _normalise(value):
    if isinstance(value, dict):
        return {k: _normalise(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_normalise(v) for v in value]
    return value


def resume_mismatch(checkpoint_spec: dict, spec: dict) -> list[str]:
    """Spec fields that differ between a checkpoint and the sim now (empty = the run can resume)."""
    return [
        key for key in RESUME_SPEC_KEYS
        if _normalise(checkpoint_spec.get(key)) != _normalise(spec.get(key))
    ]
