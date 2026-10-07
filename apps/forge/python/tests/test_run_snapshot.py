"""apps/forge/tools/run_snapshot.py: a run's readings, and a before/after comparison for the deploy gate."""

import csv
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "apps" / "forge" / "tools"))

import run_snapshot as rs  # noqa: E402

CONFIGS = ROOT / "apps" / "forge" / "python" / "configs"


def make_run(tmp_path: Path, name: str, kl: float, steps: float, found: float) -> Path:
    run = tmp_path / name
    run.mkdir()
    with (run / "metrics.csv").open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["update", "env_steps", "env_steps_per_sec", "update_seconds", "approx_kl", "entropy",
                         "shaping_scale", "elapsed_seconds"])
        for update in range(1, 31):
            writer.writerow([update, update * 24576, steps + update, 0.1, kl, 1.5, 1.0, update * 5.5])
    records = [{"update": 10, "env_steps": 245760, "policy": "learner", "episodes": 78, "score": 2.0, "stderr": 0.1,
                "summary": {"found": 0.1}},
               {"update": 30, "env_steps": 737280, "policy": "learner", "episodes": 78, "score": 2.5, "stderr": 0.05,
                "summary": {"found": found, "found_deepest": 0.5, "wall_seconds": 1.0}},
               {"update": 30, "env_steps": 737280, "policy": "learner_sampled", "episodes": 78, "score": 9.0,
                "stderr": 0.05, "summary": {"found": 0.0}}]
    (run / "eval.jsonl").write_text("\n".join(json.dumps(record) for record in records) + "\n")
    return run


def test_a_snapshot_reads_the_last_learner_evaluation_and_the_medians(tmp_path):
    run = make_run(tmp_path, "move2_seek", kl=0.014, steps=4500, found=0.8)
    data = rs.snapshot(run, CONFIGS / "move2_seek.yaml", last=10)
    assert data["update"] == 30 and data["env_steps"] == 737280 and data["rows"] == 10
    assert data["evaluation"]["update"] == 30 and data["evaluation"]["score"] == 2.5     # not the sampled arm's
    assert data["headline"]["found"] == 0.8 and data["headline"]["found_deepest"] == 0.5
    assert data["updates"]["approx_kl"] == pytest.approx(0.014)
    assert data["updates"]["env_steps_per_sec"] == pytest.approx(4500 + 25.5)            # the median of 21..30
    assert data["updates"]["shaping_scale"] == 1.0 and data["elapsed_seconds"] == pytest.approx(165.0)
    text = rs.show(data)
    assert "found" in text and "[>= 0.95: not met]" in text and "[<= 2: met]" in text


def test_compare_prints_both_readings_and_the_ratios(tmp_path, capsys):
    before = rs.snapshot(make_run(tmp_path, "a", 0.010, 4000, 0.8), CONFIGS / "move2_seek.yaml")
    after = rs.snapshot(make_run(tmp_path, "b", 0.020, 5000, 0.9), CONFIGS / "move2_seek.yaml")
    paths = []
    for name, data in (("before", before), ("after", after)):
        path = tmp_path / f"{name}.json"
        path.write_text(json.dumps(data))
        paths.append(str(path))
    assert rs.main(["--compare", *paths]) == 0
    out = capsys.readouterr().out
    approx_kl = next(line for line in out.splitlines() if line.startswith("approx_kl"))
    assert "x2.00" in approx_kl
    assert "x1.12" in next(line for line in out.splitlines() if line.startswith("found "))


def test_a_directory_without_metrics_is_an_input_error(tmp_path, capsys):
    assert rs.main([str(tmp_path)]) == 2
    assert "run_snapshot:" in capsys.readouterr().err


def test_the_command_line_saves_json(tmp_path, capsys):
    run = make_run(tmp_path, "move2_seek", 0.014, 4500, 0.8)
    out = tmp_path / "snapshot.json"
    assert rs.main([str(run), "--config", str(CONFIGS / "move2_seek.yaml"), "--json", str(out)]) == 0
    assert json.loads(out.read_text())["headline"]["found"] == 0.8
