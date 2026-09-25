import json
from types import SimpleNamespace

from animus.config import TrainConfig
from animus.evaluation import ConvergenceTracker
from animus.progress import PROGRESS_FILE, ProgressWriter, write_progress


def test_progress_is_flat_and_nonfinite_values_are_nulled(tmp_path):
    path = write_progress(tmp_path, {"phase": "training", "env_steps": 12, "entropy": float("nan"),
                                     "value_loss": float("inf"), "reward": 0.5, "ok": True})

    row = json.loads(path.read_text())
    assert path.name == PROGRESS_FILE
    assert row == {"phase": "training", "env_steps": 12, "entropy": None, "value_loss": None, "reward": 0.5,
                   "ok": 1, "nonfinite": "entropy,value_loss"}
    assert all(not isinstance(v, (dict, list)) for v in row.values())
    assert not (tmp_path / (PROGRESS_FILE + ".partial")).exists()


def test_writer_keeps_metrics_and_evaluation_between_writes(tmp_path):
    config = _config()
    spec = SimpleNamespace(scenario="stage8_duel")
    tracker = ConvergenceTracker(patience=3)
    writer = ProgressWriter(tmp_path, config, spec, resumed_update=4, resumed_env_steps=400)

    writer.training({"update": 5, "env_steps": 500, "entropy": 1.25, "env_steps_per_sec": 900.0})
    tracker.observe(10.0, 500)
    writer.evaluated(500, 10.0, 7.5, tracker)
    writer.write("training", 5, 500)
    tracker.observe(10.01, 600)
    writer.evaluated(600, 10.01, 7.5, tracker)
    row = json.loads(writer.write("evaluating", 6, 600).read_text())

    assert row["phase"] == "evaluating" and row["update"] == 6 and row["env_steps"] == 600
    assert row["total_env_steps"] == 1000 and row["resumed_update"] == 4 and row["resumed_env_steps"] == 400
    assert row["entropy"] == 1.25 and row["env_steps_per_sec"] == 900.0
    assert row["patience"] == 3 and row["eval_every"] == 100 and row["window"] == 4
    assert row["evals"] == 2 and row["last_eval_score"] == 10.01 and row["baseline_score"] == 7.5
    assert row["best_score"] == 10.0 and row["best_env_steps"] == 500 and row["evals_since_best"] == 1
    assert row["baseline"] == "fight" and row["finish_reason"] == ""


def _config() -> TrainConfig:
    config = TrainConfig(run_name="stage8_duel", total_env_steps=1000)
    config.eval.every_env_steps = 100
    config.eval.baseline = "fight"
    config.convergence.patience = 3
    config.convergence.window = 4
    return config


def test_restore_evaluation_after_resume(tmp_path):
    tracker = ConvergenceTracker(patience=2)
    tracker.observe(3.0, 100)
    tracker.observe(2.0, 200)
    writer = ProgressWriter(tmp_path, _config(), SimpleNamespace(scenario="stage8_duel"))

    writer.restore_evaluation(tracker, 1.5)
    row = json.loads(writer.write("training", 2, 200).read_text())

    assert row["last_eval_env_steps"] == 200 and row["last_eval_score"] == 2.0
    assert row["best_score"] == 3.0 and row["evals_since_best"] == 1 and row["baseline_score"] == 1.5


def test_convergence_per_class_is_reported(tmp_path):
    config = _config()
    tracker = ConvergenceTracker(patience=2)
    tracker.observe(3.0, 100)
    controller = SimpleNamespace(
        converged_layouts=lambda: ["warrior_dps"], active_layouts=lambda: ["mage_dps"],
        weakest=lambda: ("mage_dps", ["score", "kl"]), layouts={"mage_dps": SimpleNamespace(reentries=1)})
    writer = ProgressWriter(tmp_path, config, SimpleNamespace(scenario="stage8_duel"))

    writer.evaluated(100, 3.0, 1.5, tracker, controller)
    row = json.loads(writer.write("finished", 2, 200, "budget", advanced=True).read_text())

    assert row["converged_layouts"] == "warrior_dps" and row["active_layouts"] == "mage_dps"
    assert row["weakest_layout"] == "mage_dps" and row["weakest_missing"] == "score,kl" and row["reentries"] == 1
    assert row["window"] == config.convergence.window
    assert row["finish_reason"] == "budget" and row["advanced"] == 1
