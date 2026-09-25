"""Masked sampling never picks a disallowed action, and the trainer runs one update end to end."""

import copy

import numpy as np
import pytest

torch = pytest.importorskip("torch")

from animus.mappo.buffer import RolloutBuffer  # noqa: E402
from animus.mappo.networks import masked_distribution  # noqa: E402
from animus.mappo.trainer import MappoConfig, MappoTrainer  # noqa: E402


def test_masked_actions_never_sampled():
    torch.manual_seed(0)
    logits = torch.randn(2000, 4) * 5
    mask = torch.rand(2000, 4) < 0.5
    mask[:, 2] = True  # at least one allowed per row
    samples = masked_distribution(logits, mask).sample()
    assert mask[torch.arange(2000), samples].all()


def test_fully_masked_row_falls_back_to_action_zero():
    dist = masked_distribution(torch.zeros(1, 3), torch.zeros(1, 3, dtype=torch.bool))
    assert dist.sample().item() == 0


def test_trainer_update_smoke():
    envs, agents, state_dim = 4, 3, 7
    layouts = [(6, 3), (4, 5)]  # two agent layouts, padded to 6 features and 5 actions
    obs_dim, actions = 6, 5
    trainer = MappoTrainer(layouts, state_dim, MappoConfig(hidden=(16, 16), epochs=2, minibatches=2))
    buffer = RolloutBuffer(8, envs, agents, obs_dim, state_dim, actions)
    rng = np.random.default_rng(0)

    while not buffer.full:
        layout = rng.integers(0, 2, (envs, agents))
        obs = rng.random((envs, agents, obs_dim), dtype=np.float32)
        state = rng.random((envs, state_dim), dtype=np.float32)
        mask = rng.random((envs, agents, actions)) < 0.6
        mask[..., 0] = True
        mask[layout == 0, 3:] = False  # layout 0 has 3 actions
        chosen, log_probs = trainer.act(obs, mask, layout)
        assert mask[np.arange(envs)[:, None], np.arange(agents)[None, :], chosen].all()
        buffer.add_decision(obs, state, mask, layout, chosen, log_probs, trainer.value(state, obs, layout))
        done = rng.random(envs) < 0.2
        buffer.add_outcome(rng.random((envs, agents), dtype=np.float32), done, np.zeros(envs, bool),
                           np.zeros((envs, agents), np.float32))

    buffer.finish(trainer.value(state, obs, layout), 0.99, 0.95)
    stats = trainer.update(buffer)
    assert all(np.isfinite(v) for v in stats.values())


def _filled_buffer(present):
    """A 4-step, 2-env, 2-agent buffer with fixed data; agent 1's rows are marked absent where present is False."""
    envs, agents, obs_dim, state_dim, actions = 2, 2, 3, 4, 3
    buffer = RolloutBuffer(4, envs, agents, obs_dim, state_dim, actions)
    rng = np.random.default_rng(7)
    while not buffer.full:
        buffer.add_decision(rng.random((envs, agents, obs_dim), dtype=np.float32),
                            rng.random((envs, state_dim), dtype=np.float32),
                            np.ones((envs, agents, actions), bool), np.zeros((envs, agents), np.int64),
                            rng.integers(0, actions, (envs, agents)), np.zeros((envs, agents), np.float32),
                            rng.random((envs, agents), dtype=np.float32), present)
        buffer.add_outcome(rng.random((envs, agents), dtype=np.float32), np.zeros(envs, bool), np.zeros(envs, bool),
                           np.zeros((envs, agents), np.float32))
    buffer.finish(np.zeros((envs, agents), np.float32), 0.99, 0.95)
    return buffer


def test_absent_seats_are_not_samples():
    present = np.array([[True, False], [True, False]])
    buffer = _filled_buffer(present)
    flat = buffer.flat()
    assert len(flat["actions"]) == 4 * 2  # agent 0 of both envs, every step
    np.testing.assert_array_equal(flat["actions"], buffer.actions[:, :, 0].reshape(-1))
    assert buffer.mean_reward() == pytest.approx(float(buffer.rewards[:, :, 0].mean()))


def test_update_with_no_present_seat_is_a_no_op():
    trainer = MappoTrainer([(3, 3)], 4, MappoConfig(hidden=(8, 8), epochs=1, minibatches=1))
    before = {k: v.clone() for k, v in trainer.actor.state_dict().items()}
    stats = trainer.update(_filled_buffer(np.zeros((2, 2), bool)))
    assert all(v == 0.0 for v in stats.values())
    for key, value in trainer.actor.state_dict().items():
        assert torch.equal(value, before[key])


def test_act_shapes_and_greedy_pick():
    """act() flattens the batch for the actor and reshapes the answers back to [envs, agents]."""
    envs, agents, state_dim = 3, 2, 5
    layouts = [(4, 3), (6, 5)]
    trainer = MappoTrainer(layouts, state_dim, MappoConfig(hidden=(8, 8)))
    rng = np.random.default_rng(1)

    layout = rng.integers(0, 2, (envs, agents))
    obs = rng.random((envs, agents, 6), dtype=np.float32)
    mask = np.zeros((envs, agents, 5), dtype=bool)
    mask[..., 0] = True
    mask[layout == 1, 4] = True  # layout 1 alone reaches the last action

    chosen, log_probs = trainer.act(obs, mask, layout, deterministic=True)
    assert chosen.shape == (envs, agents) and log_probs.shape == (envs, agents)
    assert mask[np.arange(envs)[:, None], np.arange(agents)[None, :], chosen].all()

    # Greedy over logits is greedy over probabilities: the same rows, twice.
    again, _ = trainer.act(obs, mask, layout, deterministic=True)
    assert (chosen == again).all()


def test_update_splits_evenly_and_times_itself():
    """A sample count that does not divide by minibatches still yields exactly `minibatches` splits."""
    envs, agents, state_dim = 5, 1, 4
    layouts = [(3, 2)]
    trainer = MappoTrainer(layouts, state_dim, MappoConfig(hidden=(8, 8), epochs=2, minibatches=4))
    buffer = RolloutBuffer(3, envs, agents, 3, state_dim, 2)  # 15 samples over 4 minibatches
    rng = np.random.default_rng(2)

    while not buffer.full:
        layout = np.zeros((envs, agents), dtype=np.int64)
        obs = rng.random((envs, agents, 3), dtype=np.float32)
        state = rng.random((envs, state_dim), dtype=np.float32)
        mask = np.ones((envs, agents, 2), dtype=bool)
        chosen, log_probs = trainer.act(obs, mask, layout)
        buffer.add_decision(obs, state, mask, layout, chosen, log_probs, trainer.value(state, obs, layout))
        buffer.add_outcome(rng.random((envs, agents), dtype=np.float32), np.zeros(envs, bool),
                           np.zeros(envs, bool), np.zeros((envs, agents), dtype=np.float32))

    steps = 0
    original = trainer.actor_opt.step

    def counted(*args, **kwargs):
        nonlocal steps
        steps += 1
        return original(*args, **kwargs)

    trainer.actor_opt.step = counted
    buffer.finish(trainer.value(state, obs, layout), 0.99, 0.95)
    stats = trainer.update(buffer)

    assert steps == 2 * 4  # epochs x minibatches, with no ragged extra
    assert stats["update_compute_seconds"] > 0.0
    assert all(np.isfinite(value) for value in stats.values())


def test_rollout_networks_mirror_the_trained_ones_after_an_update():
    """The rollout copies are synced tensor by tensor, so they must still hold exactly the trained values."""
    trainer = MappoTrainer([(4, 3)], 5, MappoConfig(hidden=(8, 8), epochs=1, minibatches=1))
    envs, agents = 4, 1
    buffer = RolloutBuffer(4, envs, agents, 4, 5, 3)
    rng = np.random.default_rng(3)

    while not buffer.full:
        layout = np.zeros((envs, agents), dtype=np.int64)
        obs = rng.random((envs, agents, 4), dtype=np.float32)
        state = rng.random((envs, 5), dtype=np.float32)
        mask = np.ones((envs, agents, 3), dtype=bool)
        chosen, log_probs = trainer.act(obs, mask, layout)
        buffer.add_decision(obs, state, mask, layout, chosen, log_probs, trainer.value(state, obs, layout))
        buffer.add_outcome(rng.random((envs, agents), dtype=np.float32), np.zeros(envs, bool),
                           np.zeros(envs, bool), np.zeros((envs, agents), dtype=np.float32))

    buffer.finish(trainer.value(state, obs, layout), 0.99, 0.95)
    before = trainer._rollout_actor.trunk.layers[0].weight.clone()
    trainer.update(buffer)

    assert not torch.equal(before, trainer._rollout_actor.trunk.layers[0].weight), "the update changed nothing"
    for network, rollout in ((trainer.actor, trainer._rollout_actor), (trainer.critic, trainer._rollout_critic)):
        for (name, trained), (_, copied) in zip(network.named_parameters(), rollout.named_parameters()):
            if name.startswith(("adapters.", "state_encoder.")):
                continue  # the rollout copies carry the normalisers folded in: compared through fold_into below
            assert torch.equal(trained.cpu(), copied.cpu()), f"{name} was not mirrored"
        expected = copy.deepcopy(network).cpu()
        expected.fold_normalisation()
        for (name, folded), (_, copied) in zip(expected.named_parameters(), rollout.named_parameters()):
            if name.startswith(("adapters.", "state_encoder.")):
                assert torch.equal(folded, copied.cpu()), f"{name} was not mirrored and folded"
    if trainer.value_norm is not None:
        assert torch.equal(trainer.value_norm.running_mean.cpu(), trainer._rollout_value_norm.running_mean.cpu())


def test_advantages_are_normalised_within_each_layout():
    """Each layout's rows come out centred and scaled on their own, so one scale cannot speak for the others."""
    trainer = MappoTrainer([(4, 3), (4, 3)], 5, MappoConfig(hidden=(8, 8), per_layout_advantages=True,
                                                            min_layout_rows=4))
    layout = torch.tensor([0] * 8 + [1] * 8)
    advantages = torch.cat([torch.arange(8, dtype=torch.float32), torch.arange(8, dtype=torch.float32) * 100 + 500])

    out = trainer._normalise_advantages(advantages, layout)
    for index in (0, 1):
        rows = out[layout == index]
        assert abs(float(rows.mean())) < 1e-5
        assert abs(float(rows.std()) - 1.0) < 0.2

    # With it off, the two groups keep their very different offsets.
    trainer.config.per_layout_advantages = False
    flat = trainer._normalise_advantages(advantages, layout)
    assert float(flat[layout == 0].mean()) < -0.5 < float(flat[layout == 1].mean())


def test_small_layout_groups_fall_back_to_the_rollout_statistics():
    trainer = MappoTrainer([(4, 3), (4, 3)], 5, MappoConfig(hidden=(8, 8), per_layout_advantages=True,
                                                            min_layout_rows=8))
    layout = torch.tensor([0] * 12 + [1] * 2)  # the second layout is too thin to measure a spread with
    advantages = torch.arange(14, dtype=torch.float32)

    out = trainer._normalise_advantages(advantages, layout)
    whole = (advantages - advantages.mean()) / (advantages.std() + 1e-8)
    assert torch.allclose(out[layout == 1], whole[layout == 1])
    assert abs(float(out[layout == 0].mean())) < 1e-5


def test_value_norm_follows_a_drifting_return_scale():
    """Returns grow as the policy improves. Stats that never follow leave the critic fitting an outgrown scale."""
    from animus.mappo.valuenorm import ValueNorm

    fast, slow = ValueNorm(beta=0.99), ValueNorm(beta=0.99999)
    for _ in range(500):  # a long early run around zero ...
        for norm in (fast, slow):
            norm.update(torch.zeros(64))
    for _ in range(100):  # ... then the returns move up and stay there
        for norm in (fast, slow):
            norm.update(torch.full((64,), 50.0))

    # The normaliser that followed puts the current scale near zero; the one that averaged the whole run does not.
    assert abs(float(fast.normalize(torch.tensor([50.0])))) < abs(float(slow.normalize(torch.tensor([50.0]))))
    assert abs(float(fast.normalize(torch.tensor([50.0])))) < 1.0
