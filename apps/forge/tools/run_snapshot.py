#!/usr/bin/env python3
"""A run's current readings in one table, and a before/after comparison of two: the deploy gate's continuity check.

    run_snapshot.py RUN_DIR [--config YAML] [--last N] [--json OUT.json]     read a run directory (only reads)
    run_snapshot.py --compare BEFORE.json AFTER.json                         the two side by side, with the ratios

RUN_DIR is runs/<stage> (the live one is only read; a copy works as well). It prints the last evaluation's headline
columns (the stage's own status.headline, with the targets that stage.yaml sets), the median of the last N updates'
approx_kl, env_steps_per_sec, update_seconds and entropy, the elapsed wall seconds and the ladder rung (shaping scale).
`--json` saves the same for `--compare`: take one before the deploy (step 8 of docs/forge/deploy-gate.md) and one after
the resumed run has played a few updates and its first evaluation.

The ratios are a reading, not a verdict. After a deploy that changed no behaviour a run should continue where it was:
the evaluation's headline within its noise (the score's stderr is printed), approx_kl and entropy of the same order,
env_steps_per_sec within what the machine's load explains. A change is a reason to look at what the build changed.
"""

from __future__ import annotations

import argparse
import csv
import json
import statistics
import sys
from pathlib import Path

PYTHON_DIR = Path(__file__).resolve().parents[1] / "python"
sys.path.insert(0, str(PYTHON_DIR))

UPDATE_COLUMNS = ("approx_kl", "env_steps_per_sec", "update_seconds", "entropy", "explained_variance",
                  "shaping_scale", "cost_scale", "lr_scale", "reward_per_decision")


def last_rows(path: Path, count: int) -> list[dict]:
    """The last `count` rows of a csv, without reading it whole into memory twice."""
    if not path.is_file():
        return []
    with path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    return rows[-count:]


def number(value) -> float | None:
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def last_evaluation(path: Path) -> dict | None:
    """The last learner evaluation's record from eval.jsonl (the arms' and held-out rows are other policies)."""
    if not path.is_file():
        return None
    found = None
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        record = json.loads(line)
        if record.get("policy", "learner") == "learner":
            found = record
    return found


def headline_of(config_path: Path | None) -> tuple[list[str], dict]:
    if config_path is None or not config_path.is_file():
        return [], {}
    from animus.config import TrainConfig
    config = TrainConfig.load(config_path)
    return list(config.status.headline), dict(config.status.targets)


def snapshot(run_dir: Path, config_path: Path | None = None, last: int = 20) -> dict:
    """The run's readings as a plain dict (what --json writes)."""
    rows = last_rows(run_dir / "metrics.csv", last)
    headline, targets = headline_of(config_path)
    evaluation = last_evaluation(run_dir / "eval.jsonl")
    out: dict = {"run": run_dir.name, "headline": {}, "targets": targets, "updates": {}}
    if rows:
        out["update"] = int(float(rows[-1]["update"]))
        out["env_steps"] = int(float(rows[-1]["env_steps"]))
        out["rows"] = len(rows)
        out["elapsed_seconds"] = number(rows[-1].get("elapsed_seconds"))
        for column in UPDATE_COLUMNS:
            values = [number(row.get(column)) for row in rows]
            values = [value for value in values if value is not None]
            if values:
                out["updates"][column] = statistics.median(values)
    if evaluation is not None:
        summary = evaluation.get("summary", {})
        out["evaluation"] = {"update": evaluation.get("update"), "env_steps": evaluation.get("env_steps"),
                             "episodes": evaluation.get("episodes"), "score": evaluation.get("score"),
                             "stderr": evaluation.get("stderr")}
        names = headline or [name for name, value in summary.items() if isinstance(value, (int, float))][:20]
        out["headline"] = {name: summary.get(name) for name in names}
    return out


def met(value, target: str | None) -> str:
    if value is None or not target:
        return ""
    bound = number("".join(target.split())[2:])
    if bound is None:
        return ""
    ok = value >= bound if target.strip().startswith(">=") else value <= bound
    return f"  [{target.strip()}: {'met' if ok else 'not met'}]"


def fmt(value) -> str:
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.4g}"
    return str(value)


def show(data: dict) -> str:
    lines = [f"run {data['run']}: update {data.get('update', '-')}, env_steps {data.get('env_steps', 0):,}, "
             f"elapsed {fmt(data.get('elapsed_seconds'))} s"]
    evaluation = data.get("evaluation")
    if evaluation:
        lines.append(f"last evaluation: update {evaluation['update']}, env_steps {evaluation['env_steps']:,}, "
                     f"{evaluation['episodes']} episodes, score {fmt(evaluation['score'])} +- {fmt(evaluation['stderr'])}")
        for name, value in data["headline"].items():
            lines.append(f"  {name:28s} {fmt(value):>10s}{met(value, data['targets'].get(name))}")
    else:
        lines.append("no evaluation yet")
    lines.append(f"median of the last {data.get('rows', 0)} updates:")
    lines += [f"  {name:28s} {fmt(value):>10s}" for name, value in data["updates"].items()]
    return "\n".join(lines)


def compare(before: dict, after: dict) -> str:
    def ratio(old, new) -> str:
        if old in (None, 0) or new is None:
            return ""
        return f"x{new / old:.2f}"

    lines = [f"{'':30s}{'before':>12s}{'after':>12s}{'ratio':>9s}",
             f"{'update / env_steps':30s}{before.get('update', '-')!s:>12}{after.get('update', '-')!s:>12}",
             f"{'':30s}{before.get('env_steps', 0):>12,}{after.get('env_steps', 0):>12,}"]
    evaluations = (before.get("evaluation"), after.get("evaluation"))
    if all(evaluations):
        old, new = evaluations
        lines.append(f"{'evaluation score':30s}{fmt(old['score']):>12s}{fmt(new['score']):>12s}"
                     f"{ratio(old['score'], new['score']):>9s}   (stderr {fmt(old['stderr'])} / {fmt(new['stderr'])})")
    for name in dict.fromkeys([*before.get("headline", {}), *after.get("headline", {})]):
        old, new = before.get("headline", {}).get(name), after.get("headline", {}).get(name)
        lines.append(f"{name:30s}{fmt(old):>12s}{fmt(new):>12s}{ratio(old, new) if isinstance(old, float) else '':>9s}")
    for name in dict.fromkeys([*before.get("updates", {}), *after.get("updates", {})]):
        old, new = before.get("updates", {}).get(name), after.get("updates", {}).get(name)
        lines.append(f"{name:30s}{fmt(old):>12s}{fmt(new):>12s}{ratio(old, new):>9s}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run_dir", nargs="?", type=Path)
    parser.add_argument("--config", type=Path, help="the stage's yaml (default configs/<run>.yaml)")
    parser.add_argument("--last", type=int, default=20, help="updates the medians are taken over")
    parser.add_argument("--json", type=Path, help="also save the readings here")
    parser.add_argument("--compare", nargs=2, type=Path, metavar=("BEFORE", "AFTER"))
    args = parser.parse_args(argv)
    if args.compare:
        before, after = (json.loads(path.read_text()) for path in args.compare)
        print(compare(before, after))
        return 0
    if not args.run_dir or not (args.run_dir / "metrics.csv").is_file():
        print("run_snapshot: give a run directory with a metrics.csv", file=sys.stderr)
        return 2
    config = args.config or PYTHON_DIR / "configs" / f"{args.run_dir.name}.yaml"
    data = snapshot(args.run_dir, config, args.last)
    print(show(data))
    if args.json:
        args.json.write_text(json.dumps(data, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
