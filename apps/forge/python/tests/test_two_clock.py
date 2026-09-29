"""The two-clock seat (Component D) and the predictions it acts on (Component P): the slow loop's GAE over goals,
and a rollout-and-update cycle with every part switched on."""

import numpy as np
import torch

from animus.mappo.buffer import RolloutBuffer, compute_span_gae

from slow_gae_reference import compute_slow_gae
from animus.mappo.trainer import MappoConfig, MappoTrainer

KINDS, TARGETS = 3, 4
OWN = 40                                   # enough columns that the core health column (37) is one of them
OBS = OWN + KINDS + TARGETS + 2


def test_the_vectorised_span_gae_matches_the_reference():
    rng = np.random.default_rng(3)
    steps, envs, agents = 30, 3, 2
    rewards = rng.normal(size=(steps, envs, agents)).astype(np.float32)
    values = rng.normal(size=(steps, envs, agents)).astype(np.float32)
    final = rng.normal(size=(steps, envs, agents)).astype(np.float32)
    last = rng.normal(size=(envs, agents)).astype(np.float32)
    dones = rng.random((steps, envs)) < 0.08
    terminated = dones & (rng.random((steps, envs)) < 0.5)
    chosen = rng.random((steps, envs, agents)) < 0.3
    chosen[0] = True
    expected_adv, expected_ret = compute_slow_gae(rewards, values, dones, terminated, final, last, chosen,
                                                  np.ones_like(chosen), 0.97, 0.9)
    adv, ret, valid = compute_span_gae(rewards, values, dones, terminated, chosen, 0.97, 0.9, final, last)
    assert np.allclose(adv[chosen], expected_adv[chosen], atol=1e-5)
    assert np.allclose(ret[chosen], expected_ret[chosen], atol=1e-5)
    assert (valid == chosen).all()


def test_a_span_with_no_honest_bootstrap_is_left_out():
    """Without the values of the states a span was cut in (the slow goal value exists only at choices), a span cut
    by the rollout's end or by a truncation has no target -- rather than one bootstrapped on its own start."""
    rewards = np.ones((6, 1, 1), np.float32)
    values = np.full((6, 1, 1), 2.0, np.float32)
    chosen = np.zeros((6, 1, 1), bool)
    chosen[[0, 2, 4], 0, 0] = True
    dones = np.zeros((6, 1), bool)
    dones[1, 0] = True                  # the first span ends in a termination: it has a target (nothing follows)
    terminated = dones.copy()
    dones[3, 0] = True                  # the second in a truncation: none
    _, _, valid = compute_span_gae(rewards, values, dones, terminated, chosen, 0.9, 0.9)
    assert valid[0, 0, 0] and not valid[2, 0, 0] and not valid[4, 0, 0]   # the third is cut by the rollout's end


def rollout_with_every_part_on(**overrides):
    torch.manual_seed(0)
    settings = dict(hidden=(8, 8), recurrent_size=6, goal_count=KINDS, goal_targets=TARGETS,
                    goal_every_decisions=3, slow_goal_size=5, foresight_coef=0.1, foresight_obs_targets=True,
                    foresight_feedback=True, goal_lookahead=True, epochs=2)
    settings.update(overrides)
    config = MappoConfig(**settings)
    trainer = MappoTrainer([(OBS, 2)], 4, config)
    trainer.actor.goal_head.set_space(np.ones((KINDS, TARGETS), bool), [OWN])
    trainer._sync_rollout()
    steps, envs = 24, 2
    buffer = RolloutBuffer(steps, envs, 1, OBS, 4, 2, trainer.foresight_outputs, trainer.recurrent_size, True,
                           trainer.slow_goal_size)
    rng = np.random.default_rng(2)
    acting = trainer.acting_state(envs, 1)
    before = trainer.actor.slow_memory.weight_hh.detach().clone()
    for step in range(steps):
        obs = rng.random((envs, 1, OBS), dtype=np.float32)
        obs[..., OWN : OWN + KINDS + TARGETS] = 1.0
        obs[..., -2:] = (rng.random((envs, 1, 2)) < 0.2).astype(np.float32)
        state = rng.random((envs, 4), dtype=np.float32)
        mask = np.ones((envs, 1, 2), bool)
        layout = np.zeros((envs, 1), np.int64)
        memory = acting.memory.copy()
        actions, log_probs, values, foresight, goals, _ = trainer.act_and_value(obs, mask, layout, state,
                                                                                 state=acting)
        assert len(goals) == 5 and goals[3].shape == (envs, 1, 5)
        buffer.add_decision(obs, state, mask, layout, actions, log_probs, values, None, foresight, memory, goals)
        dones = np.array([step == 15, False])
        buffer.add_outcome(rng.random((envs, 1), dtype=np.float32), dones, dones, np.zeros((envs, 1), np.float32),
                           np.zeros((envs, 1, trainer.foresight_outputs), np.float32))
        acting.clear(dones)
    buffer.finish(np.zeros((envs, 1), np.float32), 0.99, 0.95,
                  last_foresight=np.zeros((envs, 1, trainer.foresight_outputs), np.float32),
                  foresight_gammas=(0.9, 0.99), time_scale_decisions=240.0,
                  slow_goal=(config.slow_goal_gamma, config.slow_goal_lambda),
                  obs_targets=trainer.foresight_obs_columns())
    return trainer, buffer, config, before


def test_an_unchanged_policy_has_no_kl_against_its_own_rollout():
    """The ratio compares like with like. With the two-clock seat the goal is trained on its own clock, and its log
    probability once joined the old side of the action ratio alone: every goal-choosing row's ratio was 1 / p(goal),
    and a policy that had not moved read an approx_kl of 0.2 to 2."""
    trainer, buffer, _, _ = rollout_with_every_part_on(epochs=1, actor_lr=0.0, slow_goal_lr=0.0)
    stats = trainer.update(buffer)
    assert stats["approx_kl"] < 1e-4 and stats["clip_frac"] == 0.0


def test_a_rollout_and_update_with_every_part_on():
    trainer, buffer, config, before = rollout_with_every_part_on()

    # The observation targets: health 8 decisions on, where the episode runs that far.
    horizons = len(config.foresight_horizons_seconds)
    assert buffer.foresight_valid[0, 1, 0, horizons + 1]
    assert np.isclose(buffer.foresight_targets[0, 1, 0, horizons + 1], buffer.obs[8, 1, 0, 37])
    assert buffer.slow_advantages[buffer.slow_valid].std() > 0 and not buffer.slow_valid[-1].any()

    stats = trainer.update(buffer)
    for key in ("slow_policy_loss", "slow_value_loss", "goal_entropy", "lookahead_loss", "foresight_loss"):
        assert key in stats and np.isfinite(stats[key])
    assert not torch.allclose(before, trainer.actor.slow_memory.weight_hh)
