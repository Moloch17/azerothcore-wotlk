"""Which of a parent run's checkpoints seeds the stage after it."""

import pytest

from animus.config import TrainConfig
from animus.train import SEED_MARKER, init_from_checkpoint, seed_preference


@pytest.fixture
def run(tmp_path):
    (tmp_path / "best.pt").write_text("best")
    (tmp_path / "latest.pt").write_text("latest")
    return tmp_path


def test_latest_is_what_a_run_seeds_from_by_default(run):
    """A queue advances on its own, so the default has to be the checkpoint the last stage actually trained to.
    best.pt is only rewritten by an evaluation that clears the convergence margin, and on 64-episode evaluations
    that margin is wide enough that a stage can triple its score without ever moving it."""
    assert init_from_checkpoint(str(run / "best.pt")).name == "latest.pt"
    assert TrainConfig().seed_from == "latest"


def test_best_is_still_reachable_for_a_run_that_wants_it(run):
    """What "best" buys is protection from a late regression -- an entropy collapse near the end of a stage is
    carried by latest.pt and not by best.pt. A long build may want that; it is a setting, not a removal."""
    assert init_from_checkpoint(str(run / "best.pt"), "best").name == "best.pt"
    (run / SEED_MARKER).write_text("best\n")
    assert init_from_checkpoint(str(run / "best.pt")).name == "best.pt"


def test_a_run_can_ask_for_its_latest_instead(run):
    """best.pt is only rewritten by an evaluation that clears the convergence margin. On a short run that margin is
    wide enough that best.pt can sit millions of steps behind latest.pt, and seeding from it throws that away."""
    (run / SEED_MARKER).write_text("latest\n")
    assert init_from_checkpoint(str(run / "best.pt")).name == "latest.pt"
    # and asking by the other name is the same question, so it answers the same way
    assert init_from_checkpoint(str(run / "latest.pt")).name == "latest.pt"


def test_the_marker_beats_the_configured_default_both_ways(run):
    (run / SEED_MARKER).write_text("best")
    assert init_from_checkpoint(str(run / "best.pt"), "latest").name == "best.pt"
    (run / SEED_MARKER).write_text("latest")
    assert init_from_checkpoint(str(run / "best.pt"), "best").name == "latest.pt"


def test_without_a_marker_the_configured_default_decides(run):
    assert init_from_checkpoint(str(run / "best.pt"), "latest").name == "latest.pt"
    assert init_from_checkpoint(str(run / "best.pt"), "best").name == "best.pt"


def test_the_fast_sweep_asks_for_latest_out_loud(run):
    """fast.yaml says it as well as relying on the default: a fast run is where best.pt stops moving, and it is
    where someone will look for the setting."""
    from pathlib import Path

    fast = Path(__file__).resolve().parents[1] / "configs" / "fast.yaml"
    assert "seed_from: latest" in fast.read_text()


def test_either_name_falls_back_to_the_other(tmp_path):
    """A run that never cleared the margin has no best.pt, and one killed before its first checkpoint has no
    latest.pt. Whichever it wrote is the one that seeds."""
    (tmp_path / "latest.pt").write_text("latest")
    assert init_from_checkpoint(str(tmp_path / "best.pt")).name == "latest.pt"

    only_best = tmp_path / "other"
    only_best.mkdir()
    (only_best / "best.pt").write_text("best")
    assert init_from_checkpoint(str(only_best / "best.pt"), "latest").name == "best.pt"


def test_nothing_written_yet_seeds_from_nothing(tmp_path):
    assert init_from_checkpoint(str(tmp_path / "best.pt")) is None


def test_a_junk_marker_is_ignored_rather_than_obeyed(run):
    (run / SEED_MARKER).write_text("second best, please")
    assert init_from_checkpoint(str(run / "best.pt")).name == "latest.pt"
    assert seed_preference(run) == "latest"


def test_a_path_that_is_not_a_checkpoint_name_is_taken_as_given(tmp_path):
    """finetune_from names a file directly; it is not a run directory to choose within."""
    named = tmp_path / "some-export.pt"
    named.write_text("x")
    assert init_from_checkpoint(str(named)) == named
    assert init_from_checkpoint(str(tmp_path / "missing.pt")) is None


def test_the_dashboard_and_the_learner_mean_the_same_file(run):
    """The page writes the marker and the learner reads it, and the two hold the name separately: the dashboard
    runs on the image's python, without the learner's venv, so importing animus.train there would pull in torch.
    Separate constants stay correct only if something checks they still agree."""
    from animus import dashboard

    assert dashboard.SEED_MARKER == SEED_MARKER
    assert set(dashboard.SEED_CHOICES) == {"best", "latest"}

    # What the page would show, against what the learner would actually load.
    (run / SEED_MARKER).write_text("latest\n")
    shown = dashboard.seed_state(run, {"env_steps": 6_000_000, "best_env_steps": 2_000_000})
    assert shown["choice"] == "latest"
    assert shown["resolved"] == "latest"
    assert shown["behind"] == 4_000_000
    assert init_from_checkpoint(str(run / "best.pt")).name == f"{shown['resolved']}.pt"

    (run / SEED_MARKER).unlink()
    shown = dashboard.seed_state(run, {"env_steps": 6_000_000, "best_env_steps": 2_000_000})
    assert shown["choice"] == "" and shown["resolved"] == "latest"
    assert init_from_checkpoint(str(run / "best.pt")).name == f"{shown['resolved']}.pt"


def test_the_page_and_the_learner_agree_on_the_unmarked_default(run):
    """The page holds the default separately from TrainConfig, so it can silently start describing a file the
    learner would not load. It did, the moment the default changed."""
    from animus import dashboard

    assert dashboard.SEED_DEFAULT == TrainConfig().seed_from
    shown = dashboard.seed_state(run, {"env_steps": 1, "best_env_steps": 0})["resolved"]
    assert init_from_checkpoint(str(run / "best.pt")).name == f"{shown}.pt"
