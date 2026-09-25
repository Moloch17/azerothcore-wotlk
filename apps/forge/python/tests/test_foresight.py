"""The actor's foresight heads: their targets from a rollout, and the loss they add to the update."""

import numpy as np
import torch

from animus.mappo.buffer import RolloutBuffer, compute_foresight, decisions_left
from animus.mappo.trainer import MappoConfig, MappoTrainer


def test_targets_stop_at_a_terminal_and_bootstrap_past_a_truncation():
    steps, envs, agents, horizons = 4, 2, 1, 2
    rewards = np.ones((steps, envs, agents), np.float32)
    dones = np.zeros((steps, envs), bool)
    terminated = np.zeros((steps, envs), bool)
    dones[2, 0] = terminated[2, 0] = True  # env 0 terminates at step 2
    dones[2, 1] = True  # env 1 is truncated there, worth 5 afterwards
    final = np.zeros((steps, envs, agents, horizons), np.float32)
    final[2, 1] = 5.0
    targets = compute_foresight(rewards, np.zeros((steps, envs, agents, horizons), np.float32), dones, terminated,
                                final, np.zeros((envs, agents, horizons), np.float32), (0.5, 0.5))

    assert targets[2, 0, 0, 0] == 1.0  # the terminal step is worth its own reward only
    assert targets[1, 0, 0, 0] == 1.5  # ... and the step before it, that plus half of it
    assert targets[2, 1, 0, 0] == 1.0 + 0.5 * 5.0  # truncated: the head's own estimate of what followed


def test_decisions_left_counts_to_the_end_of_the_episode():
    dones = np.zeros((4, 2), bool)
    dones[2, 0] = True
    assert list(decisions_left(dones)[:, 0]) == [2.0, 1.0, 0.0, -1.0]
    assert list(decisions_left(dones)[:, 1]) == [-1.0] * 4  # never ends in the rollout: nothing to learn from


def test_update_learns_the_heads_and_reports_their_loss():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8, 8), foresight_coef=0.5, foresight_horizons_seconds=(5.0, 30.0), epochs=2)
    trainer = MappoTrainer([(3, 2)], 4, config)
    assert trainer.foresight_outputs == 3  # two horizons and the share of the episode left

    steps, envs, agents = 4, 2, 1
    buffer = RolloutBuffer(steps, envs, agents, 3, 4, 2, trainer.foresight_outputs)
    rng = np.random.default_rng(0)
    for step in range(steps):
        obs = rng.random((envs, agents, 3), dtype=np.float32)
        state = rng.random((envs, 4), dtype=np.float32)
        mask = np.ones((envs, agents, 2), bool)
        layout = np.zeros((envs, agents), np.int64)
        actions, log_probs, values, foresight, _, _ = trainer.act_and_value(obs, mask, layout, state)
        assert foresight.shape == (envs, agents, trainer.foresight_outputs)
        buffer.add_decision(obs, state, mask, layout, actions, log_probs, values, None, foresight)
        dones = np.array([step == steps - 2, False])
        buffer.add_outcome(rng.random((envs, agents), dtype=np.float32), dones, dones,
                           np.zeros((envs, agents), np.float32),
                           np.zeros((envs, agents, trainer.foresight_outputs), np.float32))

    last_obs = rng.random((envs, agents, 3), dtype=np.float32)
    layout = np.zeros((envs, agents), np.int64)
    buffer.finish(np.zeros((envs, agents), np.float32), 0.99, 0.95,
                  last_foresight=trainer.foresight_of(last_obs, layout), foresight_gammas=(0.95, 0.99),
                  time_scale_decisions=10.0)

    flat = buffer.flat()
    assert flat["foresight_targets"].shape[1] == 3
    assert flat["foresight_valid"][:, :2].all()  # the discounted returns are always known
    assert not flat["foresight_valid"][:, 2].all()  # the share of the episode left is not

    before = trainer.actor.foresight.weight.detach().clone()
    stats = trainer.update(buffer)
    assert stats["foresight_loss"] > 0.0
    assert not torch.allclose(before, trainer.actor.foresight.weight)


def test_off_by_default():
    trainer = MappoTrainer([(3, 2)], 4, MappoConfig(hidden=(8, 8)))
    assert trainer.foresight_outputs == 0 and trainer.actor.foresight is None
    assert trainer.foresight_of(np.zeros((2, 1, 3), np.float32), np.zeros((2, 1), np.int64)) is None
