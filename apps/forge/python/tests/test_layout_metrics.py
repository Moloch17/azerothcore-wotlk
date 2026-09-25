"""Per class/build training metrics: what each layout is doing right now, not only at an evaluation."""

import csv

import numpy as np

import animus.train as train


class Layout:
    def __init__(self, name):
        self.name = name


class Spec:
    layouts = [Layout("warrior_dps"), Layout("mage_dps")]
    episode_info_names = ["killed", "timed_out"]
    episode_info_dim = 2


class Runner:
    """Only the state log_layout_rows reads, so the real method can be called on it."""

    log_layout_rows = train.TrainingRun.log_layout_rows

    def __init__(self, logger):
        self.logger = logger
        self.update = 7
        self.env_steps = 1234
        self.finished_episodes = []
        self.finished_layouts = []
        self.spec = Spec()
        self.last_layout_stats = {}
        self.lr_scale_now = 1.0
        self.frozen = np.zeros(0, dtype=np.int64)


def test_layout_rows_group_by_layout(tmp_path):
    logger = train.RunLogger(tmp_path, ["update"], append=False)
    runner = Runner(logger)
    # Two warriors (one killed, one timed out) and one mage (killed).
    runner.finished_episodes = [np.array([1.0, 0.0]), np.array([0.0, 1.0]), np.array([1.0, 0.0])]
    runner.finished_layouts = [0, 0, 1]

    runner.log_layout_rows(Spec())

    rows = list(csv.DictReader((tmp_path / "layouts.csv").open()))
    assert {r["layout"] for r in rows} == {"warrior_dps", "mage_dps"}
    by = {r["layout"]: r for r in rows}
    assert int(by["warrior_dps"]["episodes"]) == 2
    assert float(by["warrior_dps"]["episode_killed"]) == 0.5      # grouped, not averaged with the mage
    assert float(by["warrior_dps"]["episode_timed_out"]) == 0.5
    assert int(by["mage_dps"]["episodes"]) == 1
    assert float(by["mage_dps"]["episode_killed"]) == 1.0
    assert int(by["mage_dps"]["update"]) == 7
