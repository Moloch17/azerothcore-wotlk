"""The dungeon teacher's hints end once the probes beat the script (dungeon-curriculum I6), enforced in code.

The sim decides the cutoff (StageScenario::WingHintOffRung: the first rung whose probes clear more of the dungeon than
the script's runs) and says so on every ended episode of that rung or a later one (`wing_hint_off`). The learner then
holds mappo.hint_coef at 0 for the rest of the run, whatever its config asks, so the imitation loss is gone for hints
and the teacher's own rows alike. And the teacher's check configs load.
"""

from pathlib import Path
from types import SimpleNamespace

import numpy as np
import torch

from animus.config import TrainConfig
from animus.hint_cutoff import COLUMN, HintCutoff
from animus.mappo.trainer import MappoTrainer

CONFIGS = Path(__file__).resolve().parents[1] / "configs"
NAMES = ("present", "wing_rung", COLUMN, "wing_cleared_share")


def episodes(*hint_off: float) -> np.ndarray:
    rows = np.zeros((len(hint_off), len(NAMES)), dtype=np.float32)
    rows[:, 0] = 1.0
    rows[:, 2] = hint_off
    return rows


def hinted_rows():
    """Two rows of a layout with a hint block, both with a hint to imitate."""
    fake = SimpleNamespace(config=SimpleNamespace(hint_coef=1.0), hint_at=torch.tensor([3]),
                           hint_owner=torch.tensor([[0, 0, 0, 0]]), hint_block_names=["core"])
    obs = torch.zeros(2, 6)
    obs[:, 3], obs[:, 4] = 3.0, 1.0       # action 2, weight 1
    layout = torch.tensor([0, 0])
    dist = torch.distributions.Categorical(logits=torch.zeros(2, 4))
    return fake, dist, obs, layout


def test_imitation_is_switched_off_once_the_probes_beat_the_script():
    fake, dist, obs, layout = hinted_rows()
    valid = torch.ones(2, dtype=torch.bool)
    assert MappoTrainer._hint_loss(fake, dist, obs, layout, valid, {}) is not None, "imitating before the cutoff"

    cutoff = HintCutoff(NAMES, fake.config)
    assert cutoff.active
    # Runs on a rung still under the cutoff change nothing.
    assert not cutoff.observe(episodes(0.0, 0.0))
    assert fake.config.hint_coef == 1.0 and cutoff.coef() == 1.0
    # The first ended run that says its rung's imitation is off: hint_coef is 0, and the loss is gone.
    assert cutoff.observe(episodes(0.0, 1.0))
    assert fake.config.hint_coef == 0.0 and cutoff.coef() == 0.0
    assert MappoTrainer._hint_loss(fake, dist, obs, layout, valid, {}) is None, "still imitating past the cutoff"


def test_the_cutoff_holds_for_the_rest_of_the_run():
    """Episodes of another arena (a pull drill, a held-out dungeon) read 0 in the column without meaning "on", and a
    ladder falling back is the policy failing: neither brings imitation back."""
    config = SimpleNamespace(hint_coef=0.5)
    cutoff = HintCutoff(NAMES, config)
    cutoff.observe(episodes(1.0))
    assert config.hint_coef == 0.0
    for _ in range(10):
        assert not cutoff.observe(episodes(0.0, 0.0, 0.0))
    assert config.hint_coef == 0.0 and cutoff.off


def test_nothing_to_cut_without_the_column_or_without_imitation():
    config = SimpleNamespace(hint_coef=1.0)
    cutoff = HintCutoff(("present", "wing_rung"), config)
    assert not cutoff.active and not cutoff.observe(np.ones((3, 2), dtype=np.float32))
    assert config.hint_coef == 1.0
    idle = SimpleNamespace(hint_coef=0.0)
    cutoff = HintCutoff(NAMES, idle)
    assert not cutoff.active and not cutoff.observe(episodes(1.0))
    assert idle.hint_coef == 0.0 and not cutoff.off


def test_the_trainer_follows_the_cutoff():
    """The training loop hands every ended episode to the cutoff: train.py builds one over the trainer's own config."""
    import inspect

    from animus import train

    source = inspect.getsource(train)
    assert "HintCutoff(self.spec.episode_info_names, self.trainer.config)" in source
    assert "self.hint_cutoff.observe(ended[keep])" in source


def test_the_teacher_check_configs_load():
    for name in ("teacher_ragefire", "teacher_deadmines"):
        config = TrainConfig.load(CONFIGS / f"{name}.yaml")
        assert config.run_name == name
        # The teacher's runs are its own: nothing imitated, nothing self-imitated.
        assert config.mappo.hint_coef == 0.0 and config.mappo.sil_coef == 0.0
        assert config.convergence.measure == "wing_cleared_share"
        assert "wing_rejoin_seconds" in config.eval.report and "wing_rises" in config.status.headline
