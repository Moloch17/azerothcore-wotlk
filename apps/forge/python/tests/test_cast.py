"""Frozen checkpoints in the seats a script used to play (animus.cast)."""

import json
from dataclasses import asdict
from types import SimpleNamespace

import numpy as np
import pytest

pytest.importorskip("torch")

import torch  # noqa: E402

from animus import protocol as p  # noqa: E402
from animus.cast import Cast, CastActor, CastPool, CastRule, league_snapshot  # noqa: E402
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


def state_for(arenas: list[int], state_dim: int = 4) -> np.ndarray:
    state = np.zeros((len(arenas), state_dim), dtype=np.float32)
    for env, arena in enumerate(arenas):
        if arena >= 0:
            state[env, arena] = 1.0
    return state


def test_the_rule_names_the_far_side_of_mirror_and_teams_arenas():
    rule = CastRule.from_stage(stage("mirror"), 2)
    rows = rule.opponent_rows(state_for([0, 1, -1]), 2)
    assert rows.tolist() == [[False, True], [False, False], [False, False]]

    teams = CastRule.from_stage(stage("teams", team_seats=2, seats=4), 4)
    rows = teams.opponent_rows(state_for([0]), 4)
    assert rows.tolist() == [[False, False, True, True]]

    assert CastRule.from_stage({"format": 2, "arenas": [{"name": "x"}], "state": {"arena_first": 0, "arena_count": 1}}, 2) is None
    assert CastRule.from_stage(None, 2) is None


def test_static_rows_follow_presence():
    rule = CastRule.from_stage(stage(cast=[{"agent": 1, "name": "owner"}]), 2)
    present = np.array([[True, True], [True, False]])
    assert rule.static_rows(present).tolist() == [[False, True], [False, False]]


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


def pool_config(**overrides) -> CastConfig:
    config = CastConfig(opponents="league", rate_window=4, retire_above=0.75, keep_newest=1, league_size=3)
    for key, value in overrides.items():
        setattr(config, key, value)
    return config


def test_the_pool_draws_the_hard_members_most_and_retires_the_beaten(tmp_path):
    parent = checkpoint(tmp_path, "parent.pt")
    pool = CastPool(pool_config(), spec(), stage(), tmp_path, "cpu", parent)
    league_snapshot(tmp_path, parent, "step_1")
    league_snapshot(tmp_path, parent, "step_2")
    assert pool.reload() == 2 and len(pool.active()) == 3
    for _ in range(4):
        pool.record(0, 1.0)  # the parent is beaten every time
        pool.record(1, 0.0)  # the first snapshot never
    assert pool.members[0].retired and not pool.members[1].retired
    assert pool.hardest().path.name == "step_1.pt" and pool.hardest_win_rate() == pytest.approx(0.0)
    weights = pool.weights()
    assert weights[0] > weights[1]  # step_1 (hard) outweighs step_2 (unknown, 0.5)
    drawn = pool.draw(200, np.random.default_rng(1))
    assert 0 not in drawn and drawn.count(1) > drawn.count(2)
    assert pool.to_json()["active"] == 2


def test_the_newest_member_is_never_retired(tmp_path):
    parent = checkpoint(tmp_path, "parent.pt")
    pool = CastPool(pool_config(), spec(), stage(), tmp_path, "cpu", parent)
    for _ in range(4):
        pool.record(0, 1.0)
    assert not pool.members[0].retired  # it is the newest and the only member


def test_pruning_keeps_the_league_size_without_the_newest_or_hardest(tmp_path):
    parent = checkpoint(tmp_path, "parent.pt")
    pool = CastPool(pool_config(league_size=2), spec(), stage(), tmp_path, "cpu", parent)
    pool.record(0, 0.1)  # the parent is hard
    for tag in ("step_1", "step_2", "step_3"):
        league_snapshot(tmp_path, parent, tag)
    pool.reload()
    active = [member.path.name for member in pool.active()]
    assert "parent.pt" in active and "step_3.pt" in active and len(active) == 2


def test_snapshots_are_copied_once(tmp_path):
    parent = checkpoint(tmp_path, "parent.pt")
    first = league_snapshot(tmp_path, parent, "step_5")
    assert first is not None and first.name == "step_5.pt" and first.parent.name == "league"
    assert league_snapshot(tmp_path, parent, "step_5") is None
    assert league_snapshot(tmp_path, tmp_path / "missing.pt", "step_6") is None


def test_the_facade_casts_the_far_side_of_a_share_of_episodes(tmp_path):
    parent = checkpoint(tmp_path, "parent.pt")
    config = pool_config(opponents="auto", opponent_share=1.0)
    cast = Cast(config, spec(envs=4), stage(), tmp_path, "cpu", parent, seed=3)
    assert cast.enabled and cast.cast_env.all()
    step = SimpleNamespace(obs=np.zeros((4, 2, 3), dtype=np.float32), mask=np.ones((4, 2, 4), dtype=bool),
                           layout=np.zeros((4, 2), dtype=np.int64), state=state_for([0, 0, 1, -1]),
                           present=np.ones((4, 2), dtype=bool), done=np.array([True, False, False, False]),
                           episode_info=np.zeros((4, 2, 2), dtype=np.float32))
    rows = cast.rows(step)
    assert rows[:, 1].tolist() == [True, True, False, False] and not rows[:, 0].any()
    actions = cast.act(step, np.full((4, 2), 9), rows)
    assert (actions[rows] < 4).all() and (actions[~rows] == 9).all()
    step.episode_info[0, 0, 1] = 1.0  # the live seat of env 0 won
    cast.observe_ended(step, won_column=1)
    assert cast.pool.members[0].fights == 1 and cast.pool.members[0].rate > 0.5
    cast.clear(step.done)
    stats = cast.stats()
    assert stats["cast_rows"] == pytest.approx(2 / 8) and stats["cast_members"] == 1.0

    off = Cast(pool_config(opponents="auto", opponent_share=0.0), spec(envs=4), stage(), tmp_path, "cpu", parent)
    assert not off.cast_env.any() and not off.rows(step).any()


def test_a_declared_agent_is_played_by_its_checkpoint(tmp_path):
    parent = checkpoint(tmp_path, "owner.pt")
    config = CastConfig(agents={"owner": str(parent)})
    cast = Cast(config, spec(envs=2), stage(cast=[{"agent": 1, "name": "owner"}]), tmp_path, "cpu", None)
    assert cast.enabled and cast.pool is None
    step = SimpleNamespace(obs=np.zeros((2, 2, 3), dtype=np.float32), mask=np.ones((2, 2, 4), dtype=bool),
                           layout=np.zeros((2, 2), dtype=np.int64), state=state_for([1, 1]),
                           present=np.array([[True, True], [True, False]]))
    rows = cast.rows(step)
    assert rows.tolist() == [[False, True], [False, False]]
    actions = cast.act(step, np.full((2, 2), 9), rows)
    assert actions[0, 1] < 4 and actions[1, 1] == 9


def test_config_loads_a_cast_section(tmp_path):
    path = tmp_path / "c.yaml"
    path.write_text("cast:\n  opponents: league\n  opponent_share: 0.3\n  agents:\n    owner: '{runs_dir}/x/best.pt'\n")
    config = TrainConfig.load(path)
    assert config.cast.opponents == "league" and config.cast.opponent_share == 0.3
    assert config.cast.resolved_agents("runs", "r") == {"owner": "runs/x/best.pt"}
