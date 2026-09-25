"""Rollout decisions as a captured GPU graph (mappo.rollout_graphs): the same decisions as the eager path."""

import copy

import numpy as np
import pytest
import torch

from animus.mappo.trainer import MappoConfig, MappoTrainer

pytestmark = pytest.mark.skipif(not torch.cuda.is_available(), reason="rollout graphs need a GPU")

LAYOUTS = [(7, 5), (5, 3), (9, 6)]
STATE = 11


def make_trainer(**overrides) -> MappoTrainer:
    torch.manual_seed(0)
    config = MappoConfig(hidden=(16, 16), recurrent_size=8, goal_count=3, goal_every_decisions=2,
                         foresight_horizons_seconds=(1.0, 4.0), **overrides)
    return MappoTrainer(LAYOUTS, STATE, config, train_device="cuda", rollout_device="cuda")


def inputs(rng, envs=6, agents=2):
    layout = rng.integers(0, len(LAYOUTS), size=(envs, agents))
    obs = np.zeros((envs, agents, max(o for o, _ in LAYOUTS)), np.float32)
    mask = np.zeros((envs, agents, max(a for _, a in LAYOUTS)), bool)
    for index, (width, actions) in enumerate(LAYOUTS):
        rows = layout == index
        obs[rows, :width] = rng.standard_normal((int(rows.sum()), width))
        mask[rows, :actions] = rng.random((int(rows.sum()), actions)) < 0.7
    mask[..., 0] = True
    return obs, mask, layout, rng.standard_normal((envs, STATE)).astype(np.float32)


def decide(trainer, graphs: bool, state, arrays, deterministic=True):
    trainer.config.rollout_graphs = graphs
    return trainer.act_and_value(*arrays, deterministic=deterministic, state=state)


def assert_same(left, right):
    for a, b in zip(left, right):
        if isinstance(a, tuple):
            assert_same(a, b)
        elif a is None:
            assert b is None
        else:
            np.testing.assert_allclose(a, b, rtol=1e-5, atol=1e-6)


def test_the_graph_decides_as_the_eager_path_does():
    trainer = make_trainer()
    rng = np.random.default_rng(1)
    eager_state = trainer.acting_state(6, 2)
    graph_state = copy.deepcopy(eager_state)
    for _ in range(5):  # memories, goals and the goal clock carried through several decisions
        arrays = inputs(rng)
        assert_same(decide(trainer, False, eager_state, arrays), decide(trainer, True, graph_state, arrays))
        for name in ("memory", "critic_memory", "goal", "age"):
            np.testing.assert_allclose(getattr(eager_state, name), getattr(graph_state, name), rtol=1e-5, atol=1e-6)
    assert len(trainer._rollout_graphs) == 1


def test_the_graph_uses_the_weights_after_a_sync():
    """The graph read the rollout copies' tensors at capture: a sync must land the new weights in those tensors."""
    trainer = make_trainer()
    rng = np.random.default_rng(2)
    arrays = inputs(rng)
    decide(trainer, True, trainer.acting_state(6, 2), arrays)          # captured here
    with torch.no_grad():
        for parameter in [*trainer.actor.parameters(), *trainer.critic.parameters()]:
            parameter.add_(torch.randn_like(parameter) * 0.3)
        for norm in trainer.actor.norms:
            norm.update(torch.randn(32, norm.mean.shape[0], device="cuda") * 3.0 + 1.0)
    trainer._sync_rollout()
    torch.cuda.synchronize()
    assert_same(decide(trainer, False, trainer.acting_state(6, 2), arrays),
                decide(trainer, True, trainer.acting_state(6, 2), arrays))


def test_sampled_actions_respect_the_mask():
    trainer = make_trainer()
    rng = np.random.default_rng(3)
    state = trainer.acting_state(6, 2)
    seen = set()
    for _ in range(20):
        obs, mask, layout, features = inputs(rng)
        actions, log_probs, values, *_ = decide(trainer, True, state, (obs, mask, layout, features), False)
        assert mask[np.arange(6)[:, None], np.arange(2)[None, :], actions].all()
        assert np.isfinite(log_probs).all() and (log_probs <= 0).all() and np.isfinite(values).all()
        seen.update(actions.ravel().tolist())
    assert len(seen) > 1   # fresh draws each replay, not the capture's


def test_the_lean_sampler_draws_what_categorical_draws():
    """sample_logits (Gumbel-max) against Categorical on the same masked logits: the same log probabilities, the
    same argmax, draws that follow the distribution, and never a masked action."""
    from torch.distributions import Categorical

    from animus.mappo.networks import MASKED_LOGIT, log_prob_of, sample_logits

    torch.manual_seed(0)
    logits = torch.randn(4, 6, device="cuda") * 2.0
    logits[:, 4:] = MASKED_LOGIT
    reference = Categorical(logits=logits)
    choice, log_probs = sample_logits(logits, deterministic=True)
    torch.testing.assert_close(choice, reference.logits.argmax(-1))
    torch.testing.assert_close(log_probs, reference.log_prob(choice))

    draws = torch.stack([sample_logits(logits)[0] for _ in range(20000)])
    assert (draws < 4).all()
    frequencies = torch.stack([(draws == action).float().mean(0) for action in range(6)], dim=-1)
    torch.testing.assert_close(frequencies, reference.probs, atol=0.015, rtol=0.0)
    torch.testing.assert_close(log_prob_of(logits, draws[0]), reference.log_prob(draws[0]))
