"""The actor's foresight heads: their targets from a rollout (the loss in the update: test_recurrent)."""

import numpy as np

from animus.mappo.buffer import compute_foresight, decisions_left


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
