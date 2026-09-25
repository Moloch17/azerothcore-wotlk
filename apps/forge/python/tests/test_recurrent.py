"""A recurrent actor: memory carried between decisions, cleared with an episode, and learned from sequences."""

import numpy as np
import pytest
import torch

from animus.mappo.buffer import RolloutBuffer
from animus.mappo.networks import LayoutActor
from animus.mappo.trainer import MappoConfig, MappoTrainer


def fill(trainer, buffer, steps, envs, agents, obs_dim, state_dim, actions, done_at=None):
    rng = np.random.default_rng(0)
    acting = trainer.acting_state(envs, agents)
    for step in range(steps):
        obs = rng.random((envs, agents, obs_dim), dtype=np.float32)
        state = rng.random((envs, state_dim), dtype=np.float32)
        mask = np.ones((envs, agents, actions), bool)
        layout = np.zeros((envs, agents), np.int64)
        memory = acting.memory.copy() if acting.memory is not None else None
        critic_memory = acting.critic_memory.copy() if acting.critic_memory is not None else None
        chosen, log_probs, values, foresight, goals, _ = trainer.act_and_value(obs, mask, layout, state,
                                                                           state=acting)
        buffer.add_decision(obs, state, mask, layout, chosen, log_probs, values, None, foresight, memory, goals,
                            critic_memory)
        dones = np.array([step == done_at, False])
        buffer.add_outcome(rng.random((envs, agents), dtype=np.float32), dones, dones,
                           np.zeros((envs, agents), np.float32))
        acting.clear(dones)

    last = trainer.foresight_of(rng.random((envs, agents, obs_dim), dtype=np.float32),
                                np.zeros((envs, agents), np.int64), acting.memory)
    buffer.finish(np.zeros((envs, agents), np.float32), 0.99, 0.95, last_foresight=last,
                  foresight_gammas=(0.95, 0.99) if last is not None else (), time_scale_decisions=10.0)


def test_memory_is_carried_and_cleared():
    torch.manual_seed(0)
    trainer = MappoTrainer([(3, 2)], 4, MappoConfig(hidden=(8, 8), recurrent_size=6))
    acting = trainer.acting_state(2, 1)
    assert acting.memory.shape == (2, 1, 6) and not acting.memory.any()

    obs = np.ones((2, 1, 3), np.float32)
    mask = np.ones((2, 1, 2), bool)
    layout = np.zeros((2, 1), np.int64)
    trainer.act(obs, mask, layout, False, acting)
    carried = acting.memory.copy()
    assert carried.any()  # the GRU wrote something

    # The same observation with a different memory is a different decision: the policy is not stateless.
    trainer.act(obs, mask, layout, True, acting)
    assert not np.allclose(carried, acting.memory)

    acting.clear(np.array([True, False]))
    assert not acting.memory[0].any() and acting.memory[1].any()


def test_update_replays_sequences_and_learns():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8, 8), recurrent_size=6, epochs=2, minibatches=2)
    trainer = MappoTrainer([(3, 2)], 4, config)
    buffer = RolloutBuffer(6, 2, 1, 3, 4, 2, trainer.foresight_outputs, trainer.recurrent_size)
    fill(trainer, buffer, 6, 2, 1, 3, 4, 2, done_at=3)

    before = trainer.actor.memory.weight_ih.detach().clone()
    stats = trainer.update(buffer)
    assert stats["epochs_run"] == 2.0 and "policy_loss" in stats
    assert not torch.allclose(before, trainer.actor.memory.weight_ih)  # the GRU itself is trained


def test_recurrent_and_foresight_together():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8, 8), recurrent_size=4, foresight_coef=0.5, epochs=1)
    trainer = MappoTrainer([(3, 2)], 4, config)
    buffer = RolloutBuffer(4, 2, 1, 3, 4, 2, trainer.foresight_outputs, trainer.recurrent_size)
    fill(trainer, buffer, 4, 2, 1, 3, 4, 2, done_at=2)
    stats = trainer.update(buffer)
    assert stats["foresight_loss"] > 0.0


def test_a_per_minibatch_auxiliary_is_refused():
    """A flat hook cannot carry a teacher's memory through a replayed sequence, so it is refused rather than run."""
    trainer = MappoTrainer([(3, 2)], 4, MappoConfig(hidden=(8, 8), recurrent_size=4))
    buffer = RolloutBuffer(2, 2, 1, 3, 4, 2, 0, 4)
    with pytest.raises(ValueError, match="sequence-aware"):
        trainer.update(buffer, auxiliary=lambda *_: None)


def test_a_sequence_aware_auxiliary_is_replayed_in_order():
    """The recurrent update hands a sequence-aware auxiliary (animus.distill.Distiller) the whole chunk at once, in
    order, and adds its loss to the actor's."""
    torch.manual_seed(0)
    steps, envs = 4, 2
    trainer = MappoTrainer([(3, 2)], 4, MappoConfig(hidden=(8, 8), recurrent_size=4, epochs=1, minibatches=1))
    buffer = RolloutBuffer(steps, envs, 1, 3, 4, 2, 0, trainer.recurrent_size)
    fill(trainer, buffer, steps, envs, 1, 3, 4, 2, done_at=2)

    class FakeDistiller:
        coef = 0.5

        def __init__(self):
            self.sequences = []
            self.calls = 0

        def sequence_loss(self, obs, state, layout, mask, logits, dones):
            self.calls += 1
            self.sequences.append(obs.shape[1])
            # The leading shape is [steps, rows] on every argument, so the teachers can be replayed in one pass.
            assert obs.shape[0] == steps and layout.shape == (steps, obs.shape[1])
            assert dones.shape == (steps, obs.shape[1])
            assert logits.shape[:2] == (steps, obs.shape[1])
            return logits.square().mean() * self.coef, steps * obs.shape[1]

    distiller = FakeDistiller()
    stats = trainer.update(buffer, auxiliary=distiller)

    assert distiller.sequences == [envs]           # one sequence per minibatch of envs
    assert distiller.calls == 1                    # the whole chunk in one call, not one per decision
    assert stats["distill_rows"] > 0.0 and "distill_kl" in stats


def test_a_rollout_replays_from_the_memory_it_started_with():
    """The buffer keeps the memory each decision was taken with, and the update replays a sequence from the first
    one. The acting state is not reset between rollouts (animus.train.rollout), so a second rollout starts from
    whatever the first ended with, and this is what carries a plan past rollout_length."""
    torch.manual_seed(0)
    steps, envs = 3, 2
    trainer = MappoTrainer([(3, 2)], 4, MappoConfig(hidden=(8, 8), recurrent_size=4, epochs=1, minibatches=1))
    buffer = RolloutBuffer(steps, envs, 1, 3, 4, 2, 0, trainer.recurrent_size)

    acting = trainer.acting_state(envs, 1)
    acting.memory[:] = 0.5      # what an earlier rollout left behind
    rng = np.random.default_rng(0)
    for step in range(steps):
        obs = rng.random((envs, 1, 3), dtype=np.float32)
        state = rng.random((envs, 4), dtype=np.float32)
        mask = np.ones((envs, 1, 2), bool)
        layout = np.zeros((envs, 1), np.int64)
        memory = acting.memory.copy()
        chosen, log_probs, values, foresight, goals, _ = trainer.act_and_value(obs, mask, layout, state,
                                                                           state=acting)
        buffer.add_decision(obs, state, mask, layout, chosen, log_probs, values, None, foresight, memory, goals)
        dones = np.zeros(envs, bool)
        buffer.add_outcome(rng.random((envs, 1), dtype=np.float32), dones, dones,
                           np.zeros((envs, 1), np.float32), None)

    sequences = buffer.sequences()
    assert np.allclose(sequences["memory"][0], 0.5)          # the sequence starts where the last one ended
    assert not np.allclose(sequences["memory"][1], 0.5)      # and moves on from there


def test_carrying_a_sequence_matches_stepping_through_it():
    """The batched replay (encode once, then the GRU over the steps) must produce exactly what stepping decision by
    decision produced, or the update would train on features the rollout never had."""
    torch.manual_seed(0)
    steps, rows = 5, 3
    actor = LayoutActor([(4, 2)], [8, 8], recurrent_size=6)
    obs = torch.randn(steps, rows, 4)
    layout = torch.zeros(steps * rows, dtype=torch.long)
    dones = torch.zeros(steps, rows, dtype=torch.bool)
    dones[2, 1] = True                                   # an episode ends mid-sequence for one row

    memory = actor.initial_memory(rows)
    stepwise = []
    for step in range(steps):
        memory = actor.features(obs[step], layout[:rows], memory)
        stepwise.append(memory)
        memory = memory * (~dones[step]).to(memory.dtype)[:, None]

    encoded = actor.encode(obs.reshape(-1, 4), layout).reshape(steps, rows, -1)
    batched = actor.carry(encoded, actor.initial_memory(rows), dones)

    torch.testing.assert_close(batched, torch.stack(stepwise))


def test_an_evaluation_carries_one_acting_state_through_its_episodes():
    # The chooser run_evaluation is given has to hold the memory (and the goal clock) across the whole evaluation:
    # a state rebuilt per decision would score the gated, exported policy as though it remembered nothing.
    from types import SimpleNamespace

    from animus.train import TrainingRun

    torch.manual_seed(0)
    trainer = MappoTrainer([(3, 2)], 4, MappoConfig(hidden=(8, 8), recurrent_size=4, goal_count=3,
                                                    goal_every_decisions=8))
    states = []

    def acting_state(envs, agents):
        states.append(trainer.acting_state(envs, agents))
        return states[-1]

    run = SimpleNamespace(trainer=SimpleNamespace(acting_state=acting_state, act=trainer.act),
                          spec=SimpleNamespace(num_envs=2, agents_per_env=1),
                          config=SimpleNamespace(eval=SimpleNamespace(deterministic=True)),
                          _acting=lambda deterministic: TrainingRun._acting(run, deterministic))

    choose = TrainingRun.learner_actions(run)
    step = SimpleNamespace(obs=np.ones((2, 1, 3), np.float32), mask=np.ones((2, 1, 2), bool),
                           layout=np.zeros((2, 1), np.int64), done=np.zeros(2, bool))
    for _ in range(3):
        choose(step)

    assert len(states) == 1
    assert states[0].age[0, 0] == 3  # the goal clock ran on, rather than starting over on every decision


def test_critic_memory_is_carried_and_cleared():
    """The critic carries a memory of its own: the global state is a snapshot, so without one it cannot value what
    the actor remembers."""
    torch.manual_seed(0)
    trainer = MappoTrainer([(3, 2)], 4, MappoConfig(hidden=(8, 8), recurrent_size=6))
    acting = trainer.acting_state(2, 1)
    assert acting.critic_memory.shape == (2, 1, 6) and not acting.critic_memory.any()

    obs = np.ones((2, 1, 3), np.float32)
    mask = np.ones((2, 1, 2), bool)
    layout = np.zeros((2, 1), np.int64)
    state = np.ones((2, 4), np.float32)

    trainer.act_and_value(obs, mask, layout, state, state=acting)
    carried = acting.critic_memory.copy()
    assert carried.any()                                    # the critic's GRU wrote something
    assert not np.allclose(carried, acting.memory)          # ... of its own, not the actor's

    trainer.act_and_value(obs, mask, layout, state, state=acting)
    assert not np.allclose(acting.critic_memory, carried)   # the same observation values differently now

    acting.clear(np.array([True, False]))
    assert not acting.critic_memory[0].any() and acting.critic_memory[1].any()


def test_critic_value_depends_on_its_memory():
    """A value read with a carried memory differs from the same decision read cold, which is the whole point: the
    critic can tell a fight in progress from one that has just begun."""
    torch.manual_seed(0)
    trainer = MappoTrainer([(3, 2)], 4, MappoConfig(hidden=(8, 8), recurrent_size=6))
    obs = np.ones((2, 1, 3), np.float32)
    layout = np.zeros((2, 1), np.int64)
    state = np.ones((2, 4), np.float32)

    cold = trainer.value(state, obs, layout)
    warm_memory = np.ones((2, 1, 6), np.float32)
    warm = trainer.value(state, obs, layout, memory=warm_memory)
    assert not np.allclose(cold, warm)


def test_recurrent_update_replays_the_critic(tmp_path):
    """The update runs with both memories and changes the critic, so its sequences are actually being learned from."""
    torch.manual_seed(0)
    steps, envs, agents, obs_dim, state_dim, actions = 6, 2, 1, 3, 4, 2
    trainer = MappoTrainer([(obs_dim, actions)], state_dim,
                           MappoConfig(hidden=(8, 8), recurrent_size=6, epochs=2, minibatches=1))
    buffer = RolloutBuffer(steps, envs, agents, obs_dim, state_dim, actions, recurrent=6)
    fill(trainer, buffer, steps, envs, agents, obs_dim, state_dim, actions, done_at=3)

    before = trainer.critic.memory.weight_hh.detach().clone()
    stats = trainer.update(buffer)
    assert stats["value_loss"] > 0.0
    assert not torch.allclose(before, trainer.critic.memory.weight_hh)


def test_seeding_carries_the_memories(tmp_path):
    """A stage that inherits the trunk inherits how to remember with it: without this, every stage relearned its
    recurrence from scratch while keeping the features it reads."""
    from animus.bootstrap import seed_trainer

    torch.manual_seed(0)
    config = MappoConfig(hidden=(8, 8), recurrent_size=6, goal_count=3)
    trained = MappoTrainer([(3, 2)], 4, config)
    with torch.no_grad():
        for network in (trained.actor, trained.critic):
            network.memory.weight_hh.add_(0.5)

    spec = type("Spec", (), {"layouts": [type("L", (), {"name": "a", "obs_dim": 3, "num_actions": 2})()],
                             "state_dim": 4})()
    checkpoint = {"trainer": {"actor": trained.actor.state_dict(), "critic": trained.critic.state_dict()},
                  "spec": {"layouts": [{"name": "a"}]}}

    fresh = MappoTrainer([(3, 2)], 4, config)
    assert not torch.allclose(fresh.actor.memory.weight_hh, trained.actor.memory.weight_hh)
    seed_trainer(fresh, checkpoint, spec)
    assert torch.allclose(fresh.actor.memory.weight_hh, trained.actor.memory.weight_hh)
    assert torch.allclose(fresh.critic.memory.weight_hh, trained.critic.memory.weight_hh)


def test_the_taught_loss_is_added_at_the_coefficient_it_was_configured_with():
    """The distilled KL arrives already averaged over the rows it taught (animus.distill.Distiller returns
    `coef * total / rows_taught`), so the update adds it as it comes, exactly as the flat path does with its own.

    The recurrent path used to divide it a second time by the chunk length, which ran distillation at
    1/rollout_length of the coefficient the stage asked for -- 0.78% at the configured 128, and worst on the merge
    stages, whose whole purpose is to carry six teachers' behaviour into one policy."""
    torch.manual_seed(0)
    steps, envs = 4, 2
    coef, taught_kl = 0.5, 1.5

    class FixedTeacher:
        """Teaches a known KL, so what reaches the actor can be compared against what was configured."""
        def __init__(self):
            self.coef = coef

        def sequence_loss(self, obs, state, layout, mask, logits, dones):
            rows = logits.shape[0] * logits.shape[1]
            return logits.sum() * 0.0 + self.coef * taught_kl, rows

    trainer = MappoTrainer([(3, 2)], 4, MappoConfig(hidden=(8, 8), recurrent_size=4, epochs=1, minibatches=1))
    buffer = RolloutBuffer(steps, envs, 1, 3, 4, 2, 0, trainer.recurrent_size)
    fill(trainer, buffer, steps, envs, 1, 3, 4, 2, done_at=2)

    stats = trainer.update(buffer, auxiliary=FixedTeacher())
    # distill_kl is the loss divided back by the coefficient, so it is the KL the teacher actually taught --
    # not that KL scaled down by the length of the chunk it was taught over.
    assert stats["distill_kl"] == pytest.approx(taught_kl, rel=1e-4)


def test_the_first_epoch_replays_the_statistics_the_rollout_acted_through():
    """PPO's ratio is 1 at epoch 0 by construction: the stored log_probs come from the same weights the first
    forward uses. Folding the rollout into the observation normalisers *before* the epochs breaks that -- the
    rollout acted through the old statistics and the update scores it through the new ones, so the first gradient
    step reads a normaliser shift as a policy change. The statistics are folded in after the epochs instead, and
    synced to the rollout copies there, which is what makes the acting and training views of a feature identical.

    A fresh trainer is the worst case: RunningNorm passes rows through raw at count 0, so the rollout is
    unnormalised and the update would have scored it normalised. The auxiliary runs inside the minibatch, just
    after the forward, so it can say what the statistics were when the ratio was taken -- approx_kl is second
    order in the log ratio and stays near zero even when the two views disagree, so it cannot be asked this.
    """
    torch.manual_seed(0)
    steps, envs = 4, 2
    trainer = MappoTrainer([(3, 2)], 4, MappoConfig(hidden=(8, 8), recurrent_size=4, epochs=1, minibatches=1,
                                                   normalise_observations=True))
    assert float(trainer.actor.norms[0].count) == 0.0
    buffer = RolloutBuffer(steps, envs, 1, 3, 4, 2, 0, trainer.recurrent_size)
    fill(trainer, buffer, steps, envs, 1, 3, 4, 2, done_at=2)

    seen = []

    class Watcher:
        """Teaches nothing; records the statistics each minibatch's forward was taken through."""
        coef = 0.0

        def sequence_loss(self, obs, state, layout, mask, logits, dones):
            seen.append(float(trainer.actor.norms[0].count))
            return None

    trainer.update(buffer, auxiliary=Watcher())
    assert seen and seen[0] == 0.0          # the rollout's own view, which is what its log_probs came from
    assert float(trainer.actor.norms[0].count) == steps * envs   # and folded in once the epochs were done



def _carry_results(carry, cell, size, encoded, memory, dones, weight):
    cell.zero_grad()
    x = encoded.clone().requires_grad_(True)
    m = memory.clone().requires_grad_(True)
    out = carry(cell, size, x, m, dones)
    (out * weight).sum().backward()
    return [out.detach(), x.grad, m.grad] + [p.grad.clone() for p in cell.parameters()]


def _check_carry_matches_loop(steps: int, rows: int, hidden: int, size: int, seed: int = 3) -> None:
    """The fused GRU call against the step loop, both in fp32, each measured against the step loop in fp64: the
    library may sum in a different order, so it is held to the loop's own rounding error (a few times over), not to
    bit equality."""
    import copy

    from animus.mappo.networks import _carry_sequence, _carry_sequence_loop

    generator = torch.Generator().manual_seed(seed)
    cell = torch.nn.GRUCell(hidden, size).cuda()
    encoded = torch.randn(steps, rows, hidden, generator=generator).cuda()
    memory = torch.randn(rows, size, generator=generator).cuda()
    dones = torch.rand(steps, rows, generator=generator) < 0.08
    dones[-1, 0] = True             # an end on the last step
    dones[0, 1 % rows] = True       # an end on the first step
    dones[3:6, 2 % rows] = True     # ends on consecutive steps
    dones = dones.cuda()
    weight = torch.linspace(-1.0, 1.0, steps * rows * size, device="cuda").reshape(steps, rows, size)

    exact = _carry_results(_carry_sequence_loop, copy.deepcopy(cell).double(), size, encoded.double(),
                           memory.double(), dones, weight.double())
    loop = _carry_results(_carry_sequence_loop, cell, size, encoded, memory, dones, weight)
    kernel = _carry_results(_carry_sequence, cell, size, encoded, memory, dones, weight)

    names = ["output", "encoded grad", "memory grad", "weight_ih grad", "weight_hh grad", "bias_ih grad",
             "bias_hh grad"]
    for name, reference, stepped, fused in zip(names, exact, loop, kernel):
        assert fused.shape == reference.shape, name
        loop_error = float((stepped.double() - reference).abs().max())
        kernel_error = float((fused.double() - reference).abs().max())
        scale = float(reference.abs().max())
        assert kernel_error <= 8.0 * loop_error + 1e-6 * max(1.0, scale), (
            f"{name}: fused call off by {kernel_error:.3g}, the fp32 step loop by {loop_error:.3g} "
            f"(largest {scale:.3g})")


requires_gpu = pytest.mark.skipif(not torch.cuda.is_available(), reason="the fused GRU call runs on the GPU")


@requires_gpu
def test_fused_gru_matches_the_step_loop_at_the_real_size():
    _check_carry_matches_loop(steps=128, rows=16, hidden=512, size=128)


@requires_gpu
def test_fused_gru_matches_the_step_loop_at_odd_sizes():
    # Pieces of every length, including the one-step pieces between consecutive episode ends.
    _check_carry_matches_loop(steps=37, rows=21, hidden=24, size=40)


@requires_gpu
def test_fused_gru_with_no_episode_end():
    from animus.mappo.networks import _carry_sequence, _carry_sequence_loop

    cell = torch.nn.GRUCell(8, 16).cuda()
    encoded, memory = torch.randn(5, 3, 8).cuda(), torch.randn(3, 16).cuda()
    dones = torch.zeros(5, 3, dtype=torch.bool).cuda()
    torch.testing.assert_close(_carry_sequence(cell, 16, encoded, memory, dones),
                               _carry_sequence_loop(cell, 16, encoded, memory, dones), rtol=1e-4, atol=1e-5)


@requires_gpu
def test_the_update_on_two_streams_is_the_update_on_one():
    """The recurrent update runs each minibatch's actor and critic halves on two streams: scheduling only, so the
    networks it leaves are the ones a single stream leaves."""
    envs, agents, steps = 2, 1, 6
    config = MappoConfig(hidden=(16, 16), recurrent_size=8, epochs=2, minibatches=2)
    trainers = []
    for _ in range(2):
        torch.manual_seed(0)
        trainers.append(MappoTrainer([(5, 3)], 6, config, train_device="cuda", rollout_device="cpu"))
    trainer, single = trainers
    single._update_streams = (None, None)
    buffer = RolloutBuffer(steps, envs, agents, 5, 6, 3, recurrent=8)
    fill(trainer, buffer, steps, envs, agents, 5, 6, 3, done_at=2)
    torch.manual_seed(1)
    trainer.update(buffer)
    torch.manual_seed(1)
    single.update(buffer)
    for (name, got), (_, want) in zip(trainer.actor.state_dict().items(), single.actor.state_dict().items()):
        torch.testing.assert_close(got, want, rtol=1e-5, atol=1e-6, msg=lambda text: f"actor {name}: {text}")
    for (name, got), (_, want) in zip(trainer.critic.state_dict().items(), single.critic.state_dict().items()):
        torch.testing.assert_close(got, want, rtol=1e-5, atol=1e-6, msg=lambda text: f"critic {name}: {text}")
