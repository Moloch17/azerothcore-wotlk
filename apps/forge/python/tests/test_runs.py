import pytest

from animus.runs import (ARCHIVE_DIR, LATEST_CHECKPOINT, archive_run, prune_checkpoints, resume_checkpoint_path,
                         resume_mismatch)


def test_only_the_newest_checkpoints_are_kept(tmp_path):
    for update in (25, 50, 75, 100, 125):
        (tmp_path / f"checkpoint_{update:06d}.pt").write_text("x")
    (tmp_path / "latest.pt").write_text("x")
    (tmp_path / "best.pt").write_text("x")

    removed = prune_checkpoints(tmp_path, 2)

    assert sorted(p.name for p in removed) == ["checkpoint_000025.pt", "checkpoint_000050.pt", "checkpoint_000075.pt"]
    assert sorted(p.name for p in tmp_path.iterdir()) == ["best.pt", "checkpoint_000100.pt", "checkpoint_000125.pt",
                                                          "latest.pt"]
    assert prune_checkpoints(tmp_path, 0) == []


def test_an_earlier_run_is_archived(tmp_path):
    run_dir = tmp_path / "warrior_dps"
    run_dir.mkdir()
    (run_dir / "latest.pt").write_text("old")

    archived = archive_run(run_dir)

    assert archived is not None and (archived / "latest.pt").read_text() == "old"
    assert archived.parent == tmp_path / ARCHIVE_DIR
    assert run_dir.is_dir() and not any(run_dir.iterdir())

    # Every fresh start archives again.
    (run_dir / "latest.pt").write_text("new")
    second = archive_run(run_dir)
    assert second is not None and second != archived and (second / "latest.pt").read_text() == "new"


def test_nothing_to_archive(tmp_path):
    run_dir = tmp_path / "mage_dps_duel"

    assert archive_run(run_dir) is None
    assert run_dir.is_dir()
    assert not (tmp_path / ARCHIVE_DIR).exists()


def test_resume_needs_a_latest_checkpoint(tmp_path):
    run_dir = tmp_path / "stage8_duel"
    run_dir.mkdir()

    with pytest.raises(FileNotFoundError, match="latest.pt"):
        resume_checkpoint_path(run_dir)

    (run_dir / LATEST_CHECKPOINT).write_text("ckpt")
    assert resume_checkpoint_path(run_dir) == run_dir / LATEST_CHECKPOINT


def spec_dict(**changes) -> dict:
    spec = {
        "version": 5, "num_envs": 64, "agents_per_env": 1, "obs_dim": 10, "state_dim": 6, "num_actions": 4,
        "episode_info_dim": 3, "tick_ms": 50, "decision_ticks": 2, "episode_seconds": 60, "scenario": "stage8_duel",
        "layouts": ({"name": "warrior_dps", "obs_dim": 10, "num_actions": 4},),
        "episode_info_names": ("damage", "dps", "level"),
    }
    spec.update(changes)
    return spec


def test_resume_allows_pool_and_timing_changes():
    saved = spec_dict()
    now = spec_dict(num_envs=16, decision_ticks=4, episode_seconds=90)
    now["layouts"] = [dict(saved["layouts"][0])]  # a list after asdict, a tuple in an older checkpoint

    assert resume_mismatch(saved, now) == []


def test_resume_refuses_changed_shapes():
    saved = spec_dict()
    now = spec_dict(state_dim=7, layouts=({"name": "warrior_dps", "obs_dim": 11, "num_actions": 4},))

    assert resume_mismatch(saved, now) == ["state_dim", "layouts"]
