"""forge status shows each stage's own measures (the user, 2026-10-05): a stage's config names them in status.headline,
with optional targets, and the learner carries them to the sim through progress.json."""

import json
from pathlib import Path
from types import SimpleNamespace

import pytest

from animus.config import StatusConfig, TrainConfig, from_dict
from animus.progress import ProgressWriter

CONFIGS = Path(__file__).resolve().parents[1] / "configs"


def test_targets_flatten_for_the_sim_and_bad_ones_are_refused_at_load():
    status = StatusConfig(headline=("arrived", "arrive_seconds"), targets={"arrived": ">= 0.95",
                                                                          "arrive_seconds": "<=18"})
    assert status.target_text() == "arrived>=0.95;arrive_seconds<=18"
    with pytest.raises(ValueError, match="not in status.headline"):
        StatusConfig(headline=("arrived",), targets={"died": "<= 0.1"})
    with pytest.raises(ValueError, match="'>= x' or '<= x'"):
        StatusConfig(headline=("arrived",), targets={"arrived": "> 0.9"})
    with pytest.raises(ValueError, match="unknown config keys"):
        from_dict(TrainConfig, {"status": {"headlines": ["arrived"]}})


def test_m1_reads_its_runs_by_arrival_time_and_precision():
    config = TrainConfig.load(CONFIGS / "move1_controls.yaml")
    assert config.status.headline[:3] == ("arrived", "arrive_seconds", "time_ratio")
    assert config.status.targets["arrived"] == ">= 0.95"
    assert config.convergence.measure == "arrived"


def test_progress_carries_the_headline_and_its_evaluation_means(tmp_path):
    config = TrainConfig()
    config.run_name = "move1_controls"
    config.status = StatusConfig(headline=("arrived", "arrive_seconds"), targets={"arrived": ">= 0.95"})
    writer = ProgressWriter(tmp_path, config, SimpleNamespace(scenario="move1_controls"))
    tracker = SimpleNamespace(history=[(5, 1.0)], best=1.0, best_env_steps=5, evals_since_best=0)
    writer.evaluated(5, 1.0, None, tracker, summary={"arrived": 0.5, "arrive_seconds": 21.0, "died": 0.0})
    writer.training({"episode_arrived": 0.4})
    written = json.loads(writer.write("training", 1, 5).read_text())
    assert written["status_headline"] == "arrived,arrive_seconds"
    assert written["status_targets"] == "arrived>=0.95"
    assert written["eval_arrived"] == 0.5 and written["eval_arrive_seconds"] == 21.0
    assert "eval_died" not in written and written["episode_arrived"] == 0.4
