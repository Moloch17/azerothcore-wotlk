"""A layout that decides on a slower clock than the seats: the director.

Two things have to hold. Its call stands between its decisions, so the seats have something steady enough to
act on -- a call that moves every decision is one nothing can follow. And its transitions run from the decision
it took to the next one it takes, carrying the rewards in between, so what it is credited with is what its call
actually governed rather than the 250 ms that happened to follow it.
"""

import numpy as np
import torch

from animus.mappo.buffer import RolloutBuffer, compute_slow_gae
from animus.mappo.trainer import MappoConfig, MappoTrainer


def trainer_with_a_slow_layout(**overrides):
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8, 8), slow_layout="director", slow_every_decisions=3, **overrides)
    # Two layouts: a seat (index 0) and the director (index 1), which is the one on the slow clock.
    return MappoTrainer([(3, 4), (3, 4)], 4, config, slow_layout=1)


def test_a_call_stands_until_the_clock_comes_round():
    trainer = trainer_with_a_slow_layout()
    acting = trainer.acting_state(1, 2)
    obs = np.ones((1, 2, 3), np.float32)
    mask = np.ones((1, 2, 4), bool)
    state = np.zeros((1, 4), np.float32)
    layout = np.array([[0, 1]], np.int64)      # agent 0 is a seat, agent 1 the director

    calls, chosen_at = [], []
    for _ in range(7):
        actions, _, _, _, _, chosen = trainer.act_and_value(obs, mask, layout, state, state=acting)
        calls.append(int(actions[0, 1]))
        chosen_at.append(bool(chosen[0, 1]))

    # It chooses on decisions 0, 3 and 6 and keeps what it said in between.
    assert chosen_at == [True, False, False, True, False, False, True]
    assert calls[0] == calls[1] == calls[2]
    assert calls[3] == calls[4] == calls[5]

    # The seat beside it is never held: every one of its decisions is its own.
    assert bool(chosen[0, 0])


def test_a_held_decision_is_not_a_sample():
    """The recurrence still has to replay a held decision -- the env moved on -- but the agent chose nothing
    there, so it must not reach the loss or the advantages."""
    trainer = trainer_with_a_slow_layout()
    buffer = RolloutBuffer(steps=4, envs=1, agents=2, obs_dim=3, state_dim=4, num_actions=4)
    acting = trainer.acting_state(1, 2)
    obs = np.ones((1, 2, 3), np.float32)
    mask = np.ones((1, 2, 4), bool)
    state = np.zeros((1, 4), np.float32)
    layout = np.array([[0, 1]], np.int64)

    for _ in range(4):
        actions, log_probs, values, _, _, chosen = trainer.act_and_value(obs, mask, layout, state, state=acting)
        buffer.add_decision(obs, state, mask, layout, actions, log_probs, values, None, None, None, None, None,
                            chosen)
        buffer.add_outcome(np.zeros((1, 2), np.float32), np.zeros(1, bool), np.zeros(1, bool),
                           np.zeros((1, 2), np.float32))

    # The seat is a sample at every step; the director only where it chose.
    assert buffer.samples[:, 0, 0].tolist() == [True, True, True, True]
    assert buffer.samples[:, 0, 1].tolist() == [True, False, False, True]
    # It was present throughout: held is not absent.
    assert buffer.valid[:, 0, 1].all()


def test_a_transition_carries_the_rewards_of_the_span_it_governed():
    """Hand-checkable: two spans of two decisions, gamma 0.5 and lambda 1."""
    rewards = np.array([[[1.0]], [[2.0]], [[3.0]], [[4.0]]], np.float32)
    values = np.array([[[10.0]], [[0.0]], [[20.0]], [[0.0]]], np.float32)
    dones = np.zeros((4, 1), bool)
    chosen = np.array([[[True]], [[False]], [[True]], [[False]]])
    slow = np.ones((4, 1, 1), bool)

    advantages, returns = compute_slow_gae(
        rewards, values, dones, dones, np.zeros_like(rewards), np.array([[5.0]], np.float32),
        chosen, slow, gamma=0.5, gae_lambda=1.0)

    # Second span: rewards 3 + 4, bootstrapped on the value after the rollout.
    second = (3.0 + 4.0) + 0.5 * 5.0 - 20.0
    # First span: rewards 1 + 2, bootstrapped on the value at the next decision it took, plus the trace.
    first = (1.0 + 2.0) + 0.5 * 20.0 - 10.0 + 0.5 * second
    assert np.isclose(advantages[2, 0, 0], second)
    assert np.isclose(advantages[0, 0, 0], first)
    assert np.isclose(returns[0, 0, 0], first + 10.0)
    # Nothing is credited to the decisions it did not take.
    assert advantages[1, 0, 0] == 0.0 and advantages[3, 0, 0] == 0.0


def test_credit_does_not_cross_the_end_of_an_episode():
    """A span that ends in a termination is worth its own rewards and nothing that follows."""
    rewards = np.array([[[1.0]], [[2.0]], [[3.0]], [[4.0]]], np.float32)
    values = np.array([[[10.0]], [[0.0]], [[20.0]], [[0.0]]], np.float32)
    dones = np.array([[False], [True], [False], [False]])
    terminated = np.array([[False], [True], [False], [False]])
    chosen = np.array([[[True]], [[False]], [[True]], [[False]]])
    slow = np.ones((4, 1, 1), bool)

    advantages, _ = compute_slow_gae(
        rewards, values, dones, terminated, np.zeros_like(rewards), np.array([[5.0]], np.float32),
        chosen, slow, gamma=0.5, gae_lambda=1.0)

    # 1 + 2, nothing after a termination, and no trace from the episode that follows.
    assert np.isclose(advantages[0, 0, 0], 3.0 - 10.0)
