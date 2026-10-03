"""Which of a parent run's checkpoints seeds the stage after it (TrainConfig.seed_from)."""

from pathlib import Path

import pytest

from animus.config import TrainConfig
from animus.train import init_from_checkpoint


@pytest.fixture
def run(tmp_path):
    (tmp_path / "best.pt").write_text("best")
    (tmp_path / "latest.pt").write_text("latest")
    return tmp_path


def test_seed_from_picks_the_checkpoint_whichever_name_is_asked_for(run):
    """latest by default: a queue advances on its own and has to hand the next stage what the last one trained to,
    and best.pt is only rewritten by an evaluation that clears the convergence margin. "best" stays reachable, for a
    long build that wants protection from a late regression. fast.yaml says latest out loud, where people look."""
    assert TrainConfig().seed_from == "latest"
    for asked in ("best.pt", "latest.pt"):
        assert init_from_checkpoint(str(run / asked)).name == "latest.pt"
        assert init_from_checkpoint(str(run / asked), "best").name == "best.pt"
    fast = Path(__file__).resolve().parents[1] / "configs" / "fast.yaml"
    assert "seed_from: latest" in fast.read_text()


def test_either_name_falls_back_to_the_other_and_other_paths_are_taken_as_given(tmp_path):
    """A run that never cleared the margin has no best.pt, and one killed before its first checkpoint has no
    latest.pt: whichever it wrote seeds. finetune_from names a file directly, not a run to choose within."""
    (tmp_path / "latest.pt").write_text("latest")
    assert init_from_checkpoint(str(tmp_path / "best.pt"), "best").name == "latest.pt"
    only_best = tmp_path / "other"
    only_best.mkdir()
    (only_best / "best.pt").write_text("best")
    assert init_from_checkpoint(str(only_best / "latest.pt")).name == "best.pt"
    assert init_from_checkpoint(str(tmp_path / "empty" / "best.pt")) is None

    named = tmp_path / "some-export.pt"
    named.write_text("x")
    assert init_from_checkpoint(str(named)) == named
    assert init_from_checkpoint(str(tmp_path / "missing.pt")) is None
