"""Frozen checkpoints in the seats the stage declares cast (animus.cast)."""

from types import SimpleNamespace

import numpy as np
import pytest

pytest.importorskip("torch")

import torch  # noqa: E402

from animus import protocol as p  # noqa: E402
from animus.cast import Cast, CastActor, snapshot  # noqa: E402
from animus.config import CastConfig, MappoConfig, TrainConfig  # noqa: E402
from animus.mappo.trainer import MappoTrainer  # noqa: E402
from animus.train import save_checkpoint  # noqa: E402

LAYOUTS = (p.Layout("warrior_dps", 3, 4), p.Layout("mage_dps", 3, 4))


def spec(envs: int = 3, agents: int = 2) -> p.Spec:
    return p.Spec(version=p.PROTOCOL_VERSION, num_envs=envs, agents_per_env=agents, obs_dim=3, state_dim=4,
                  num_actions=4, episode_info_dim=2, goal_count=0, tick_ms=50, decision_ticks=1,
                  episode_seconds=1, scenario="fake", layouts=LAYOUTS, episode_info_names=("present", "won"))


def stage(plan: str = "mirror", team_seats: int = 1, seats: int = 2, cast=()) -> dict:
    return {"format": 3, "seats": seats, "state": {"arena_first": 0, "arena_count": 2},
            "arenas": [{"name": "arena_1v1", "plan": plan, "team_seats": team_seats},
                       {"name": "duel", "plan": "solo", "team_seats": 0}],
            "cast": list(cast), "layouts": {}}


def checkpoint(tmp_path, name="parent.pt"):
    config = TrainConfig()
    config.mappo = MappoConfig(hidden=(8, 8), recurrent_size=4, goal_count=0)
    trainer = MappoTrainer([(3, 4), (3, 4)], 4, config.mappo, "cpu", "cpu")
    path = tmp_path / name
    save_checkpoint(path, trainer, config, spec(), 0, 0, {"stage": stage()})
    return path


def test_the_actor_picks_only_legal_actions_and_keeps_its_memory(tmp_path):
    actor = CastActor(checkpoint(tmp_path), spec(), stage(), "cpu")
    envs, agents = 3, 2
    obs = np.random.rand(envs, agents, 3).astype(np.float32)
    mask = np.zeros((envs, agents, 4), dtype=bool)
    mask[..., 1] = True  # the only legal action
    layout = np.zeros((envs, agents), dtype=np.int64)
    rows = np.zeros((envs, agents), dtype=bool)
    rows[:, 1] = True
    fallback = np.full((envs, agents), 3, dtype=np.int64)
    actions = actor.act(obs, mask, layout, rows, fallback)
    assert actions[:, 0].tolist() == [3, 3, 3] and actions[:, 1].tolist() == [1, 1, 1]
    assert actor.fallback_rows == 0
    assert np.abs(actor.memory[:, 1]).sum() > 0.0 and np.abs(actor.memory[:, 0]).sum() == 0.0
    actor.clear(np.array([True, False, False]))
    assert np.abs(actor.memory[0, 1]).sum() == 0.0 and np.abs(actor.memory[1, 1]).sum() > 0.0

    mask[:, 1, :] = False  # nothing legal for the cast rows: the fallback stands
    actions = actor.act(obs, mask, layout, rows, fallback)
    assert actions[:, 1].tolist() == [3, 3, 3] and actor.fallback_rows == 3


def test_a_layout_the_checkpoint_lacks_keeps_the_live_action(tmp_path):
    actor = CastActor(checkpoint(tmp_path), spec(), stage(), "cpu")
    obs = np.zeros((1, 2, 3), dtype=np.float32)
    mask = np.ones((1, 2, 4), dtype=bool)
    layout = np.array([[5, 5]])  # no such layout in the checkpoint
    rows = np.array([[False, True]])
    actions = actor.act(obs, mask, layout, rows, np.array([[2, 2]]))
    assert actions.tolist() == [[2, 2]] and actor.fallback_rows == 1


def test_snapshots_are_copied_once(tmp_path):
    parent = checkpoint(tmp_path, "parent.pt")
    first = snapshot(tmp_path, parent, "step_5", "partners")
    assert first is not None and first.name == "step_5.pt" and first.parent.name == "partners"
    assert snapshot(tmp_path, parent, "step_5", "partners") is None
    assert snapshot(tmp_path, tmp_path / "missing.pt", "step_6", "partners") is None


def test_a_declared_agent_is_played_by_its_checkpoint(tmp_path):
    parent = checkpoint(tmp_path, "owner.pt")
    config = CastConfig(agents={"owner": str(parent)})
    cast = Cast(config, spec(envs=2), stage(cast=[{"agent": 1, "name": "owner"}]), "cpu")
    assert list(cast.statics) == [1]
    step = SimpleNamespace(obs=np.zeros((2, 2, 3), dtype=np.float32), mask=np.ones((2, 2, 4), dtype=bool),
                           layout=np.zeros((2, 2), dtype=np.int64),
                           present=np.array([[True, True], [True, False]]))
    rows = cast.rows(step)
    assert rows.tolist() == [[False, True], [False, False]]
    actions = cast.act_and_look(step, np.full((2, 2), 9), rows)[0]
    assert actions[0, 1] < 4 and actions[1, 1] == 9
    assert cast.stats()["cast_rows"] == pytest.approx(1 / 3)


def test_config_loads_a_cast_section(tmp_path):
    path = tmp_path / "c.yaml"
    path.write_text("cast:\n  agents:\n    owner: '{runs_dir}/x/best.pt'\n")
    config = TrainConfig.load(path)
    assert config.cast.resolved_agents("runs", "r") == {"owner": "runs/x/best.pt"}
