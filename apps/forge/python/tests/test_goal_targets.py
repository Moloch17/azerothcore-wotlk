"""Goals as a kind and a target (GoalHead): masked by the sim's goal block, chosen again when a goal ends, and the
same in an exported model as in the learner."""

import numpy as np
import torch

from animus.export import export_layouts, read_amdl, reference_decide
from animus.mappo.networks import LayoutActor
from animus.mappo.trainer import MappoConfig, MappoTrainer

KINDS, TARGETS = 3, 4
OWN = 5                                  # observation columns before the goal block
BLOCK = KINDS + TARGETS + 1              # kinds there, targets there, ended
OBS = OWN + BLOCK
ACCEPTS = np.array([[1, 1, 0, 0],        # kind 0 about nothing or target 1
                    [0, 0, 1, 1],        # kind 1 about target 2 or 3
                    [1, 0, 0, 0]], bool)  # kind 2 about nothing


def goal_obs(kinds, targets, ended=False, rows=1):
    obs = np.zeros((rows, OBS), np.float32)
    obs[:, :OWN] = 0.3
    obs[:, OWN : OWN + KINDS] = kinds
    obs[:, OWN + KINDS : OWN + KINDS + TARGETS] = targets
    obs[:, -1] = float(ended)
    return obs


def actor():
    torch.manual_seed(0)
    net = LayoutActor([(OBS, 2)], [8, 8], goal_count=KINDS, goal_targets=TARGETS)
    net.goal_head.set_space(ACCEPTS, [OWN])
    return net


def test_only_goals_the_block_offers_and_the_kind_accepts_can_be_drawn():
    net = actor()
    # Kind 1 is there, and of its targets only 3; kind 0 is not there.
    obs = torch.tensor(goal_obs([0, 1, 0], [1, 0, 0, 1]))
    layout = torch.zeros(1, dtype=torch.long)
    with torch.no_grad():
        features = net.features(obs, layout)
        probs = net.goal_distribution(features, obs, layout).probs[0].numpy()
    allowed = {g for g in range(KINDS * TARGETS) if probs[g] > 0}
    # Kind 1 about target 3 (goal 7), and always the first goal (Fight about no one) as the fallback.
    assert allowed == {0, 1 * TARGETS + 3}


def test_an_ended_goal_is_chosen_again_before_its_clock():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8, 8), goal_count=KINDS, goal_targets=TARGETS, goal_every_decisions=100)
    trainer = MappoTrainer([(OBS, 2)], 4, config)
    trainer.actor.goal_head.set_space(ACCEPTS, [OWN])
    trainer._sync_rollout()
    acting = trainer.acting_state(1, 1)
    mask = np.ones((1, 1, 2), bool)
    layout = np.zeros((1, 1), np.int64)
    state = np.zeros((1, 4), np.float32)

    chosen = []
    for ended in (False, False, True, False):
        obs = goal_obs([1, 1, 1], [1, 1, 1, 1], ended)[None]
        record = trainer.act_and_value(obs, mask, layout, state, state=acting)[4]
        chosen.append(bool(record[2][0, 0]))
    # The first decision chooses (a new episode), the clock is far off, and the ended goal forces a choice.
    assert chosen == [True, False, True, False]
    assert acting.age[0, 0] == 2


def test_the_exported_goal_head_matches_the_learner(tmp_path):
    net = actor()
    spec = {"scenario": "stage", "layouts": [{"name": "warrior_dps", "obs_dim": OBS, "num_actions": 2}]}
    export_layouts(net.state_dict(), spec, tmp_path, tmp_path, goal_every=4)
    model = read_amdl(tmp_path / "stage.amdl")
    assert model["goals"]["block_at"] == OWN and model["goals"]["accepts"].shape == (KINDS, TARGETS)

    obs = goal_obs([1, 1, 0], [1, 1, 1, 0])[0]
    state = {}
    action, logits = reference_decide(model, obs, np.ones(2, bool), state=state)
    with torch.no_grad():
        obs_t = torch.tensor(obs)[None]
        layout = torch.zeros(1, dtype=torch.long)
        features = net.features(obs_t, layout)
        goal = net.goal_distribution(features, obs_t, layout).logits.argmax(dim=-1)
        expected = net.action_distribution(features, layout, torch.ones(1, 2, dtype=torch.bool), goal)
    assert state["goal"] == int(goal)
    torch_logits = expected.logits[0].numpy()
    assert np.allclose(logits - logits[0], torch_logits - torch_logits[0], atol=1e-4)
    assert action == int(expected.logits[0].argmax())
