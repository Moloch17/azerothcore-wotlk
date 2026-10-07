"""The two-clock seat's goals are credited on their own clock: a transition runs from the decision that chose a goal to
the next one it chooses, carrying the rewards in between, so what it is credited with is what the goal actually
governed rather than the one decision that happened to follow it."""

import numpy as np

from animus.mappo.buffer import compute_span_gae


def test_a_transition_carries_the_rewards_of_the_span_it_governed():
    """Hand-checkable: two spans of two decisions, gamma 0.5 and lambda 1. The open span at the rollout's end has no
    honest bootstrap (the slow value exists only where a goal was chosen), so it is left out and the first span, which
    the second one bootstraps, carries no trace from it."""
    rewards = np.array([[[1.0]], [[2.0]], [[3.0]], [[4.0]]], np.float32)
    values = np.array([[[10.0]], [[0.0]], [[20.0]], [[0.0]]], np.float32)
    dones = np.zeros((4, 1), bool)
    chosen = np.array([[[True]], [[False]], [[True]], [[False]]])

    advantages, returns, valid = compute_span_gae(rewards, values, dones, dones, chosen, gamma=0.5, gae_lambda=1.0)

    assert valid[0, 0, 0] and not valid[2, 0, 0]
    # First span: rewards 1 + 2, bootstrapped on the value at the next decision it took (no trace from the open one).
    first = (1.0 + 2.0) + 0.5 * 20.0 - 10.0
    assert np.isclose(advantages[0, 0, 0], first)
    assert np.isclose(returns[0, 0, 0], first + 10.0)
    # Nothing is credited to the decisions it did not take.
    assert advantages[1, 0, 0] == 0.0 and advantages[3, 0, 0] == 0.0


def test_credit_does_not_cross_the_end_of_an_episode():
    """A span that ends in a termination is worth its own rewards and nothing that follows, and has a target."""
    rewards = np.array([[[1.0]], [[2.0]], [[3.0]], [[4.0]]], np.float32)
    values = np.array([[[10.0]], [[0.0]], [[20.0]], [[0.0]]], np.float32)
    dones = np.array([[False], [True], [False], [False]])
    terminated = np.array([[False], [True], [False], [False]])
    chosen = np.array([[[True]], [[False]], [[True]], [[False]]])

    advantages, _, valid = compute_span_gae(rewards, values, dones, terminated, chosen, gamma=0.5, gae_lambda=1.0)

    # 1 + 2, nothing after a termination, and no trace from the episode that follows.
    assert np.isclose(advantages[0, 0, 0], 3.0 - 10.0)
    assert valid[0, 0, 0]
