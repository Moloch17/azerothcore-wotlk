"""GAE against hand-computed values, including truncation bootstrap and termination."""

import numpy as np

from animus.mappo.buffer import compute_gae


def reference_gae(rewards, values, dones, terminated, final_values, last_value, gamma, lam):
    """Straightforward scalar implementation for a single env/agent."""
    steps = len(rewards)
    advantages = [0.0] * steps
    running = 0.0
    for t in reversed(range(steps)):
        if dones[t]:
            next_value = 0.0 if terminated[t] else final_values[t]
            running = 0.0
        else:
            next_value = last_value if t == steps - 1 else values[t + 1]
        delta = rewards[t] + gamma * next_value - values[t]
        running = delta + gamma * lam * running
        advantages[t] = running
    return np.array(advantages, dtype=np.float32)


def run_case(dones, terminated, gamma=0.9, lam=0.8):
    rng = np.random.default_rng(3)
    steps = len(dones)
    rewards = rng.random(steps).astype(np.float32)
    values = rng.random(steps).astype(np.float32)
    final_values = rng.random(steps).astype(np.float32)
    last_value = np.float32(0.37)

    advantages, returns = compute_gae(
        rewards[:, None, None],
        values[:, None, None],
        np.array(dones)[:, None],
        np.array(terminated)[:, None],
        final_values[:, None, None],
        np.array([[last_value]], dtype=np.float32),
        gamma,
        lam,
    )

    expected = reference_gae(rewards, values, dones, terminated, final_values, last_value, gamma, lam)
    np.testing.assert_allclose(advantages[:, 0, 0], expected, rtol=1e-5, atol=1e-6)
    np.testing.assert_allclose(returns[:, 0, 0], expected + values, rtol=1e-5, atol=1e-6)


def test_no_episode_boundary():
    run_case([False] * 5, [False] * 5)


def test_truncation_bootstraps_from_final_value():
    run_case([False, True, False, False, True], [False, False, False, False, False])


def test_termination_does_not_bootstrap():
    run_case([False, True, False, True, False], [False, True, False, True, False])


def test_single_step_truncation_by_hand():
    # One step, truncated: advantage = r + gamma * V(final) - V(s).
    advantages, _ = compute_gae(
        np.array([[[1.0]]], dtype=np.float32),
        np.array([[[0.5]]], dtype=np.float32),
        np.array([[True]]),
        np.array([[False]]),
        np.array([[[2.0]]], dtype=np.float32),
        np.array([[9.0]], dtype=np.float32),  # must be ignored: the episode ended
        gamma=0.9,
        gae_lambda=0.95,
    )
    assert np.isclose(advantages[0, 0, 0], 1.0 + 0.9 * 2.0 - 0.5)
