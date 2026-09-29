"""The two-clock seat (Component D) and the predictions it acts on (Component P): the slow loop's GAE over goals,
and a rollout-and-update cycle with every part switched on."""

import numpy as np
import torch

from animus.mappo.buffer import RolloutBuffer, compute_slow_gae, compute_span_gae
from animus.mappo.trainer import MappoConfig, MappoTrainer

KINDS, TARGETS = 3, 4
OWN = 40                                   # enough columns that the core health column (37) is one of them
OBS = OWN + KINDS + TARGETS + 2


def test_the_vectorised_span_gae_matches_the_reference():
    rng = np.random.default_rng(3)
    steps, envs, agents = 30, 3, 2
    rewards = rng.normal(size=(steps, envs, agents)).astype(np.float32)
    values = rng.normal(size=(steps, envs, agents)).astype(np.float32)
    dones = rng.random((steps, envs)) < 0.08
    terminated = dones & (rng.random((steps, envs)) < 0.5)
    chosen = rng.random((steps, envs, agents)) < 0.3
    chosen[0] = True
    # The reference bootstraps truncations and the rollout's end from these; the vectorised version uses the value of
    # the last chosen decision before them, so give the reference the same.
    latest = np.zeros_like(values)
    carried = np.zeros((envs, agents), np.float32)
    for t in range(steps):
        carried = np.where(chosen[t], values[t], carried)
        latest[t] = carried
        carried = np.where(dones[t][:, None], 0.0, carried)
    expected_adv, expected_ret = compute_slow_gae(rewards, values, dones, terminated, latest, latest[-1], chosen,
                                                  np.ones_like(chosen), 0.97, 0.9)
    adv, ret = compute_span_gae(rewards, values, dones, terminated, chosen, 0.97, 0.9)
    assert np.allclose(adv[chosen], expected_adv[chosen], atol=1e-5)
    assert np.allclose(ret[chosen], expected_ret[chosen], atol=1e-5)


def test_a_rollout_and_update_with_every_part_on():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8, 8), recurrent_size=6, goal_count=KINDS, goal_targets=TARGETS,
                         goal_every_decisions=3, slow_goal_size=5, foresight_coef=0.1, foresight_obs_targets=True,
                         foresight_feedback=True, goal_lookahead=True, epochs=2)
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

    # The observation targets: health 8 decisions on, where the episode runs that far.
    horizons = len(config.foresight_horizons_seconds)
    assert buffer.foresight_valid[0, 1, 0, horizons + 1]
    assert np.isclose(buffer.foresight_targets[0, 1, 0, horizons + 1], buffer.obs[8, 1, 0, 37])
    assert buffer.slow_advantages[buffer.goal_chosen].std() > 0

    stats = trainer.update(buffer)
    for key in ("slow_policy_loss", "slow_value_loss", "goal_entropy", "lookahead_loss", "foresight_loss"):
        assert key in stats and np.isfinite(stats[key])
    assert not torch.allclose(before, trainer.actor.slow_memory.weight_hh)
