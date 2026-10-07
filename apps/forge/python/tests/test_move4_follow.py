"""M4 follow (dungeon-curriculum I5, I4): the learner config loads with its own measures, its ladder steps on its gate
alone and no cost ladder holds it (the dungeon plan's rule: nothing waits on a cost ladder or a plateau), its per-event
columns are weighted by their events, and seeding it from an M2 checkpoint carries the movement and starts the party
frames block fresh."""

from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest
import torch

from animus.bootstrap import seed_trainer
from animus.config import TrainConfig
from animus.episode_means import PER_EVENT, means
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout

CONFIGS = Path(__file__).resolve().parents[1] / "configs"
LAYOUT = "warrior"


def test_the_config_loads_with_its_own_measures():
    config = TrainConfig.load(CONFIGS / "move4_follow.yaml")
    assert config.run_name == "move4_follow"
    assert config.convergence.measure == "follow_kept_share"
    assert config.fade.gate_metric == "follow_kept_share" and config.layout_sampling.metric == "follow_kept_share"
    assert config.mappo.gamma == pytest.approx(0.999)
    assert "follow_kept_share" in config.status.headline and "rejoin_seconds" in config.status.headline
    # M2's targets dropped, M4's own read.
    assert "found" not in config.status.targets and config.status.targets["follow_kept_share"] == ">= 0.9"


def test_no_ladder_waits_on_a_plateau_or_the_cost_ladder():
    """The fade (the leader's ladder) steps on its gate alone, and there is no cost ladder: the noise prices are at full
    price from the start (M1's stall: a fade held at x1 by a plateau rule and a cost ladder)."""
    config = TrainConfig.load(CONFIGS / "move4_follow.yaml")
    assert config.fade.enabled and config.fade.require_plateau is False
    assert config.fade.gate_metric and config.fade.gate_value > 0.0
    assert config.fade.rungs == (1.0, 0.5, 0.25, 0.0)
    assert config.costs.enabled is False


def test_the_per_event_columns_are_weighted_by_their_events():
    names = ("regroup_stops", "regroups", "regroup_share", "regroup_seconds", "rises", "rejoins", "rejoined",
             "rejoin_seconds")
    for column, count in (("regroup_share", "regroup_stops"), ("regroup_seconds", "regroups"),
                          ("rejoined", "rises"), ("rejoin_seconds", "rejoins")):
        assert PER_EVENT[column] == count
    episodes = np.array([
        # 10 stops, 9 regroups in 4 s; one death, risen and rejoined in 30 s.
        [10, 9, 0.9, 4.0, 1, 1, 1.0, 30.0],
        # 2 stops, 1 regroup in 10 s; no death.
        [2, 1, 0.5, 10.0, 0, 0, 0.0, 0.0],
    ], dtype=np.float32)
    out = dict(zip(names, means(episodes, names)))
    assert out["regroup_share"] == pytest.approx((0.9 * 10 + 0.5 * 2) / 12)
    assert out["regroup_seconds"] == pytest.approx((4.0 * 9 + 10.0) / 10)
    assert out["rejoined"] == pytest.approx(1.0)        # the episode with no rise does not count
    assert out["rejoin_seconds"] == pytest.approx(30.0)


def stage(blocks):
    obs = actions = 0
    spans, names = [], []
    for block, revision, block_actions, features in blocks:
        entry = {"name": block, "obs": [obs, features], "actions": [actions, len(block_actions)]}
        if revision:
            entry["revision"] = revision
        spans.append(entry)
        names += block_actions
        obs += features
        actions += len(block_actions)
    return {"layouts": {LAYOUT: {"obs_dim": obs, "num_actions": actions, "blocks": spans, "action_names": names}}}


CORE = ("core", 1, ["core_0", "core_1", "core_2"], 4)
MOVE = ("move", 5, [f"move_key_{index}" for index in range(5)], 57)
GOAL = ("goal", 0, [], 2)
FRAMES = 4 * 10
M2 = stage([CORE, MOVE, GOAL])
M4 = stage([CORE, MOVE, ("party_frames", 0, [], FRAMES), GOAL])


def dims(stage_json):
    entry = stage_json["layouts"][LAYOUT]
    return entry["obs_dim"], entry["num_actions"]


def test_seeding_from_m2_carries_the_movement_and_starts_the_party_frames_fresh():
    torch.manual_seed(0)
    old = MappoTrainer([dims(M2)], 4, MappoConfig(hidden=(16, 16)))
    new = MappoTrainer([dims(M4)], 4, MappoConfig(hidden=(16, 16)))
    checkpoint = {"trainer": old.state_dict(), "stage": M2,
                  "spec": {"layouts": [{"name": LAYOUT, "obs_dim": dims(M2)[0], "num_actions": dims(M2)[1]}]}}
    layout = Layout(LAYOUT, *dims(M4))
    assert seed_trainer(new, checkpoint, SimpleNamespace(layouts=(layout,), state_dim=4), M4) == [LAYOUT]
    new_w = new.actor.state_dict()["adapters.0.weight"]
    old_w = old.actor.state_dict()["adapters.0.weight"]
    width = CORE[3] + MOVE[3]
    # Core and move carry as blocks, the goal block after the new one too.
    torch.testing.assert_close(new_w[:, :width], old_w[:, :width])
    torch.testing.assert_close(new_w[:, -GOAL[3]:], old_w[:, -GOAL[3]:])
    # The party frames are new: their join starts at zero, so the seeded policy plays as M2's did until it learns them.
    assert torch.count_nonzero(new_w[:, width:width + FRAMES]) == 0
    # The move block's actions are its own as they were.
    torch.testing.assert_close(new.actor.state_dict()["heads.0.weight"], old.actor.state_dict()["heads.0.weight"])
