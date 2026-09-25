"""When the observation normalisers are allowed to move, on the flat update path.

The recurrent path's counterpart is in test_recurrent.py; the two paths must agree, because every configured stage
sets `recurrent_size` and so takes the recurrent one, while the flat one is what the tests mostly exercise.
"""

import pytest
import torch

from animus.mappo.buffer import RolloutBuffer
from animus.mappo.trainer import MappoConfig, MappoTrainer


def rollout(trainer, buffer, steps=4, envs=2, obs_dim=3, state_dim=5, actions=2):
    for _ in range(steps):
        obs = torch.randn(envs, 1, obs_dim).numpy()
        state = torch.randn(envs, state_dim).numpy()
        mask = torch.ones(envs, 1, actions, dtype=torch.bool).numpy()
        layout = torch.zeros(envs, 1, dtype=torch.long).numpy()
        chosen, log_probs = trainer.act(obs, mask, layout)
        buffer.add_decision(obs, state, mask, layout, chosen, log_probs, trainer.value(state, obs, layout))
        buffer.add_outcome(torch.ones(envs, 1).numpy(), torch.zeros(envs, dtype=torch.bool).numpy(),
                           torch.zeros(envs, dtype=torch.bool).numpy(), torch.zeros(envs, 1).numpy())
    buffer.finish(torch.zeros(envs, 1).numpy(), 0.99, 0.95)


def test_the_rollout_is_scored_through_the_statistics_it_acted_through():
    """The stored log_probs are the denominator of every PPO ratio in the update, so the first forward has to see
    the observations exactly as the rollout saw them. Folding the rollout into the normalisers before the epochs
    made the ratio something other than 1 at epoch 0 -- a shift in the statistics read as a change in the policy.
    """
    torch.manual_seed(0)
    trainer = MappoTrainer([(3, 2)], 5, MappoConfig(hidden=(8,), epochs=1, minibatches=1,
                                                   normalise_observations=True))
    buffer = RolloutBuffer(4, 2, 1, 3, 5, 2)
    rollout(trainer, buffer)

    # The auxiliary runs inside the minibatch, just after the forward, so it can say what the statistics were
    # when the ratio was taken. approx_kl is second order in the log ratio and stays near zero even when the two
    # views disagree, so it corroborates rather than decides.
    seen = []

    def watch(data, idx, dist):
        seen.append(float(trainer.actor.norms[0].count))
        return None

    stats = trainer.update(buffer, auxiliary=watch)
    assert seen and seen[0] == 0.0
    assert stats["approx_kl"] == pytest.approx(0.0, abs=1e-6)


def test_the_statistics_still_move_and_reach_the_rollout_copies():
    """Deferring the fold must not skip it: the normalisers still describe the rollout afterwards, and the copies
    the next rollout acts through carry the same numbers."""
    torch.manual_seed(0)
    trainer = MappoTrainer([(3, 2)], 5, MappoConfig(hidden=(8,), epochs=1, minibatches=1,
                                                   normalise_observations=True))
    buffer = RolloutBuffer(4, 2, 1, 3, 5, 2)
    rollout(trainer, buffer)
    trainer.update(buffer)

    assert float(trainer.actor.norms[0].count) == 8          # four decisions of two envs
    torch.testing.assert_close(trainer._rollout_actor.norms[0].mean, trainer.actor.norms[0].mean)
    torch.testing.assert_close(trainer._rollout_actor.norms[0].count, trainer.actor.norms[0].count)


def test_the_rollout_copies_fold_the_normalisers_into_their_adapters():
    """The rollout copies bypass their normalisers and carry them in the adapters' weights instead: the same
    distributions and values as the trained networks, which keep them apart."""
    torch.manual_seed(0)
    layouts = [(6, 3), (4, 2)]
    trainer = MappoTrainer(layouts, 5, MappoConfig(hidden=(8, 8), recurrent_size=4))
    obs, state = torch.randn(64, 6) * 3.0 + 2.0, torch.randn(64, 5) * 5.0 - 1.0
    layout = torch.arange(64) % 2
    for index, (dim, _) in enumerate(layouts):
        rows = layout == index
        trainer.actor.norms[index].update(obs[rows, :dim])
        trainer.critic.norms[index].update(obs[rows, :dim])
    trainer.critic.state_norm.update(state)
    trainer.sync_rollout()

    assert all(norm.bypass for norm in trainer._rollout_actor.norms)
    assert not any(norm.bypass for norm in trainer.actor.norms)
    mask = torch.ones(64, 3, dtype=torch.bool)
    mask[layout == 1, 2] = False
    memory = torch.randn(64, 4)
    with torch.no_grad():
        trained = trainer.actor(obs, layout, mask, memory=memory).logits
        folded = trainer._rollout_actor(obs, layout, mask, memory=memory).logits
        torch.testing.assert_close(folded, trained, rtol=1e-4, atol=1e-4)
        trained_value = trainer.critic(state, obs, layout, memory=memory)
        folded_value = trainer._rollout_critic(state, obs, layout, memory=memory)
        torch.testing.assert_close(folded_value, trained_value, rtol=1e-4, atol=1e-4)


def test_dense_rollout_layers_match_the_per_layout_ones():
    """A GPU rollout copy runs every layout's adapter and head as one product (densify); it must choose from the same
    distribution and value the same state as the per-layout layers, whatever the padding past a layout's width."""
    import copy

    torch.manual_seed(1)
    layouts = [(6, 3), (4, 5), (5, 2)]
    trainer = MappoTrainer(layouts, 7, MappoConfig(hidden=(8, 8), recurrent_size=4, goal_count=0))
    rows = 30
    layout = torch.arange(rows) % 3
    obs = torch.randn(rows, 6)
    for index, (dim, _) in enumerate(layouts):
        obs[layout == index, dim:] = 99.0      # whatever sits past a layout's width must not matter
    state, memory = torch.randn(rows, 7), torch.randn(rows, 4)
    mask = torch.rand(rows, 5) < 0.7
    for index, (_, actions) in enumerate(layouts):
        mask[layout == index, actions:] = False

    actor, critic = copy.deepcopy(trainer.actor), copy.deepcopy(trainer.critic)
    dense_actor, dense_critic = copy.deepcopy(actor), copy.deepcopy(critic)
    dense_actor.densify(6)
    dense_critic.densify(6)
    with torch.no_grad():
        expected = actor(obs, layout, mask, memory=memory).logits
        got = dense_actor(obs, layout, mask, memory=memory).logits
        allowed = mask | ~mask.any(dim=-1, keepdim=True)
        torch.testing.assert_close(got[allowed], expected[allowed], rtol=1e-5, atol=1e-5)
        torch.testing.assert_close(dense_critic(state, obs, layout, memory=memory),
                                   critic(state, obs, layout, memory=memory), rtol=1e-5, atol=1e-5)
