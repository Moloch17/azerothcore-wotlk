"""The goal head: a goal chosen on its own clock, kept in between, and scored as part of the decision."""

import numpy as np
import torch

from animus.mappo.buffer import RolloutBuffer
from animus.mappo.trainer import MappoConfig, MappoTrainer


def trainer_with_goals(**overrides):
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8, 8), goal_count=4, goal_every_decisions=3, **overrides)
    return MappoTrainer([(3, 2)], 4, config)


def test_a_goal_is_kept_until_its_clock_comes_round():
    trainer = trainer_with_goals()
    acting = trainer.acting_state(2, 1)
    obs = np.ones((2, 1, 3), np.float32)
    mask = np.ones((2, 1, 2), bool)
    layout = np.zeros((2, 1), np.int64)

    goals = []
    chosen = []
    for _ in range(7):
        _, _, _, _, record, _ = trainer.act_and_value(obs, mask, layout, np.zeros((2, 4), np.float32),
                                                  state=acting)
        goals.append(record[0].copy())
        chosen.append(record[2].copy())

    # Chosen on the first decision and every third after it; kept in between.
    assert [bool(step[0, 0]) for step in chosen] == [True, False, False, True, False, False, True]
    assert (goals[1] == goals[0]).all() and (goals[2] == goals[0]).all()

    # A new episode chooses afresh.
    acting.clear(np.array([True, False]))
    assert acting.age[0, 0] == 0


def test_the_goal_is_part_of_the_decision_and_is_learned():
    trainer = trainer_with_goals(epochs=2, minibatches=2)
    buffer = RolloutBuffer(6, 2, 1, 3, 4, 2, goals=True)
    rng = np.random.default_rng(0)
    acting = trainer.acting_state(2, 1)
    for step in range(6):
        obs = rng.random((2, 1, 3), dtype=np.float32)
        state = rng.random((2, 4), dtype=np.float32)
        mask = np.ones((2, 1, 2), bool)
        layout = np.zeros((2, 1), np.int64)
        actions, log_probs, values, _, goals, _ = trainer.act_and_value(obs, mask, layout, state, state=acting)
        buffer.add_decision(obs, state, mask, layout, actions, log_probs, values, None, None, None, goals)
        dones = np.array([step == 4, False])
        buffer.add_outcome(rng.random((2, 1), dtype=np.float32), dones, dones, np.zeros((2, 1), np.float32))
        acting.clear(dones)
    buffer.finish(np.zeros((2, 1), np.float32), 0.99, 0.95)

    assert buffer.flat()["goal_chosen"].any()
    before = trainer.actor.goal_head.weight.detach().clone()
    stats = trainer.update(buffer)
    assert stats["policy_loss"] is not None
    assert not torch.allclose(before, trainer.actor.goal_head.weight)  # the chooser learns from the same returns


def test_goals_and_memory_together():
    trainer = trainer_with_goals(recurrent_size=4, epochs=1)
    buffer = RolloutBuffer(4, 2, 1, 3, 4, 2, 0, trainer.recurrent_size, goals=True)
    rng = np.random.default_rng(1)
    acting = trainer.acting_state(2, 1)
    for _ in range(4):
        obs = rng.random((2, 1, 3), dtype=np.float32)
        state = rng.random((2, 4), dtype=np.float32)
        mask = np.ones((2, 1, 2), bool)
        layout = np.zeros((2, 1), np.int64)
        memory = acting.memory.copy()
        actions, log_probs, values, _, goals, _ = trainer.act_and_value(obs, mask, layout, state, state=acting)
        buffer.add_decision(obs, state, mask, layout, actions, log_probs, values, None, None, memory, goals)
        dones = np.zeros(2, bool)
        buffer.add_outcome(rng.random((2, 1), dtype=np.float32), dones, dones, np.zeros((2, 1), np.float32))
    buffer.finish(np.zeros((2, 1), np.float32), 0.99, 0.95)
    assert trainer.update(buffer)["epochs_run"] == 1.0


def rollout_with_goals(trainer, steps=4, recurrent=False):
    buffer = RolloutBuffer(steps, 2, 1, 3, 4, 2, 0, trainer.recurrent_size if recurrent else 0, goals=True)
    rng = np.random.default_rng(2)
    acting = trainer.acting_state(2, 1)
    for _ in range(steps):
        obs = rng.random((2, 1, 3), dtype=np.float32)
        state = rng.random((2, 4), dtype=np.float32)
        mask = np.ones((2, 1, 2), bool)
        layout = np.zeros((2, 1), np.int64)
        memory = acting.memory.copy() if recurrent else None
        actions, log_probs, values, _, goals, _ = trainer.act_and_value(obs, mask, layout, state, state=acting)
        buffer.add_decision(obs, state, mask, layout, actions, log_probs, values, None, None, memory, goals)
        dones = np.zeros(2, bool)
        buffer.add_outcome(rng.random((2, 1), dtype=np.float32), dones, dones, np.zeros((2, 1), np.float32))
    buffer.finish(np.zeros((2, 1), np.float32), 0.99, 0.95)
    return buffer


def test_the_reported_entropy_is_the_actions_alone():
    # Two actions and four goals: a reported entropy that folded the goal head in would run past ln(2), and the
    # entropy floor would then read a collapsing action policy as a healthy one.
    for recurrent in (False, True):
        trainer = trainer_with_goals(epochs=1, recurrent_size=4 if recurrent else 0)
        stats = trainer.update(rollout_with_goals(trainer, recurrent=recurrent))
        assert stats["entropy"] <= np.log(2) + 1e-4
        assert 0.0 < stats["goal_entropy"] <= np.log(4) + 1e-4
