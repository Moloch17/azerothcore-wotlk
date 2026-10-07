"""The dungeon teacher's hints end once the probes beat the script (dungeon-curriculum I6), enforced in code.

The sim decides the cutoff per rung (StageScenario::WingHintOffRung: the first rung whose probes clear more of the
dungeon than that rung's reference runs) and says it on every ended training run of a whole dungeon
(`wing_hint_off_rung`, beside the run's `wing_rung`). The learner holds mappo.hint_coef at 0 exactly while the runs'
rung is at or past it, whatever its config asks, and imitates again if the ladder falls back below it. And the
teacher's check configs load.
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


def episodes(*runs: tuple[int, int]) -> np.ndarray:
    """Ended runs, each (its rung, the sim's cutoff rung: -1 none, -2 not a whole dungeon's training run)."""
    rows = np.zeros((len(runs), len(NAMES)), dtype=np.float32)
    rows[:, 0] = 1.0
    for i, (rung, cutoff) in enumerate(runs):
        rows[i, 1], rows[i, 2] = rung, cutoff
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
    # No cutoff yet, or a rung below it: nothing changes.
    assert not cutoff.observe(episodes((3, -1), (4, -1)))
    assert not cutoff.observe(episodes((4, 6)))
    assert fake.config.hint_coef == 1.0 and cutoff.coef() == 1.0
    # The runs reach the cutoff rung (HINTOFF 6 at rung 6): hint_coef is 0, and the loss is gone.
    assert cutoff.observe(episodes((5, 6), (6, 6)))
    assert fake.config.hint_coef == 0.0 and cutoff.coef() == 0.0
    assert MappoTrainer._hint_loss(fake, dist, obs, layout, valid, {}) is None, "still imitating past the cutoff"
    # ... and every later rung.
    assert not cutoff.observe(episodes((8, 6)))
    assert fake.config.hint_coef == 0.0


def test_a_ladder_falling_back_below_the_cutoff_has_its_hints_again():
    """hint_coef is 0 exactly for rungs at or past the cutoff: the ladder stepping back below it brings imitation back
    (the sim writes its hints there again), and stepping on to it again takes it away."""
    config = SimpleNamespace(hint_coef=0.5)
    cutoff = HintCutoff(NAMES, config)
    assert cutoff.observe(episodes((7, 7)))
    assert config.hint_coef == 0.0
    assert cutoff.observe(episodes((7, 7), (6, 7)))
    assert config.hint_coef == 0.5 and not cutoff.off
    # A lower cutoff found on the way back (the probes beat that rung's script too): off again there.
    assert cutoff.observe(episodes((6, 6)))
    assert config.hint_coef == 0.0


def test_runs_that_are_no_dungeons_training_runs_say_nothing():
    """Another arena's episode, a drill or an evaluation (-2) moves nothing either way."""
    config = SimpleNamespace(hint_coef=1.0)
    cutoff = HintCutoff(NAMES, config)
    cutoff.observe(episodes((5, 5)))
    assert config.hint_coef == 0.0
    for _ in range(5):
        assert not cutoff.observe(episodes((0, -2), (0, -2)))
    assert config.hint_coef == 0.0 and cutoff.off


def test_nothing_to_cut_without_the_column_or_without_imitation():
    config = SimpleNamespace(hint_coef=1.0)
    cutoff = HintCutoff(("present", "wing_rung"), config)
    assert not cutoff.active and not cutoff.observe(np.ones((3, 2), dtype=np.float32))
    assert config.hint_coef == 1.0
    idle = SimpleNamespace(hint_coef=0.0)
    cutoff = HintCutoff(NAMES, idle)
    assert not cutoff.active and not cutoff.observe(episodes((5, 5)))
    assert idle.hint_coef == 0.0 and not cutoff.off


def test_the_trainer_follows_the_cutoff():
    """The training loop hands every ended episode to the cutoff: train.py builds one over the trainer's own config."""
    import inspect

    from animus import train

    source = inspect.getsource(train)
    assert "HintCutoff(self.spec.episode_info_names, self.trainer.config)" in source
    assert "self.hint_cutoff.observe(ended[keep])" in source
    # Reference runs (the teacher playing every seat) are hint data, out of the training statistics.
    assert 'ended[:, reference] <= 0.5' in source


def test_the_teacher_check_configs_load():
    for name in ("teacher_ragefire", "teacher_deadmines"):
        config = TrainConfig.load(CONFIGS / f"{name}.yaml")
        assert config.run_name == name
        # The teacher's runs are its own: nothing imitated, nothing self-imitated.
        assert config.mappo.hint_coef == 0.0 and config.mappo.sil_coef == 0.0
        assert config.convergence.measure == "wing_cleared_share"
        assert "wing_rejoin_seconds" in config.eval.report and "wing_rises" in config.status.headline
        # C3's (combat3_survive.yaml): its fade rungs on the teacher's measure, and no cost ladder.
        assert config.costs.enabled is False
        assert config.fade.enabled and config.fade.rungs == (1.0, 0.5, 0.25, 0.0)
        assert config.fade.gate_metric == "wing_cleared_share" and config.fade.require_plateau is False
        assert "survived" not in config.status.targets
