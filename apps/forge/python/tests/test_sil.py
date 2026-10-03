"""Self-imitation (animus.mappo.sil, peak-play W5): the best episodes kept with their Monte Carlo returns, the terms
pull only where the return beat the critic, and off there is no term."""

import numpy as np
import torch

from animus.mappo.buffer import RolloutBuffer
from animus.mappo.sil import SelfImitation, sil_policy_loss, sil_value_loss
from animus.mappo.trainer import MappoConfig, MappoTrainer

LAYOUTS = [(6, 3)]
STATE = 4


def rollout(trainer: MappoTrainer, rewards: float, steps: int = 4, envs: int = 2) -> RolloutBuffer:
    """A rollout whose env 0 ends an episode at its last decision paying `rewards` a decision, always action 2."""
    rng = np.random.default_rng(0)
    buffer = RolloutBuffer(steps, envs, 1, 6, STATE, 3, 0, trainer.recurrent_size)
    buffer.reset()
    acting = trainer.acting_state(envs, 1)
    for t in range(steps):
        obs = rng.random((envs, 1, 6), dtype=np.float32)
        mask = np.ones((envs, 1, 3), bool)
        state = rng.random((envs, STATE), dtype=np.float32)
        memory = acting.memory.copy()
        _, log_probs, values, *_ = trainer.act_and_value(obs, mask, np.zeros((envs, 1), np.int64), state,
                                                         state=acting)
        actions = np.full((envs, 1), 2)
        buffer.add_decision(obs, state, mask, np.zeros((envs, 1), np.int64), actions, log_probs, values, None, None,
                            memory)
        done = np.array([t == steps - 1, False])
        buffer.add_outcome(np.full((envs, 1), rewards, np.float32), done, done, np.zeros((envs, 1), np.float32))
    buffer.finish(np.zeros((envs, 1), np.float32), 0.9, 0.95)
    buffer.ended_episodes.append((steps - 1, np.array([0]), np.array([rewards])))
    return buffer


def trainer(sil_coef: float) -> MappoTrainer:
    torch.manual_seed(0)
    config = MappoConfig(hidden=(16, 16), recurrent_size=4, epochs=1, minibatches=1, sil_coef=sil_coef,
                         sil_episodes=2, sil_batch=2, actor_lr=0.0, critic_lr=0.0)
    return MappoTrainer(LAYOUTS, STATE, config)


def test_the_best_episodes_are_kept_with_their_returns():
    sil = SelfImitation(episodes=1)
    model = trainer(0.0)
    assert sil.collect(rollout(model, 1.0), gamma=0.5) == 1
    [tail] = sil.tails
    np.testing.assert_allclose(tail["returns"][:, 0], [1.875, 1.75, 1.5, 1.0])     # Monte Carlo to the end
    assert sil.collect(rollout(model, 0.5), gamma=0.5) == 0                       # worse than the one kept
    assert sil.collect(rollout(model, 2.0), gamma=0.5) == 1 and sil.tails[0]["score"] == 2.0
    batch = sil.batch(4)
    assert batch["obs"].shape == (4, 1, 1, 6) and batch["memory"].shape == (1, 1, 4)


def test_only_a_return_that_beat_the_critic_is_imitated():
    """Row 0 did better than the critic expects, row 1 worse, row 2 better but not a sample of the policy: only row 0
    adds to either term or pulls on the policy."""
    target = torch.tensor([2.0, -1.0, 3.0])
    expected = torch.tensor([1.0, 0.0, 0.0])
    log_probs = torch.tensor([-0.5, -0.5, -0.5], requires_grad=True)
    counted = torch.tensor([1.0, 1.0, 0.0])
    policy, better = sil_policy_loss(target, expected, log_probs, counted)
    policy.backward()
    assert torch.isclose(policy, torch.tensor(0.25)) and torch.isclose(better, torch.tensor(0.5))
    assert torch.equal(log_probs.grad, torch.tensor([-0.5, 0.0, 0.0]))       # more likely, row 0 alone
    predicted = torch.tensor([1.0, 0.0, 0.0], requires_grad=True)
    sil_value_loss(target, predicted, counted).backward()
    assert torch.equal(predicted.grad, torch.tensor([-0.5, 0.0, 0.0]))       # pulled up, row 0 alone


def test_on_the_update_imitates_the_kept_runs_and_off_it_has_no_term():
    on = trainer(1.0)
    stats = on.update(rollout(on, 5.0))
    assert stats["sil_episodes"] == 1.0 and stats["sil_better_share"] > 0.0 and stats["sil_policy_loss"] > 0.0
    off = trainer(0.0)
    assert off.sil is None and "sil_policy_loss" not in off.update(rollout(off, 5.0))
