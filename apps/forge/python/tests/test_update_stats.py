"""The recurrent update's reported statistics, on a fixed seed, against what the update reported before its
per-minibatch statistics were kept on the device and read once (peak-play E4, B2): the same numbers, only fewer
synchronisations. The reference, update_stats_reference.json, was written by the update as it was before that change
(write_reference below); regenerate it only for a change that is meant to move these numbers (last: 2026-10-07, the
hint block and its imitation loss removed with the dungeon teacher)."""

import json
from pathlib import Path

import numpy as np
import pytest
import torch

from animus.mappo.buffer import RolloutBuffer
from animus.mappo.trainer import MappoConfig, MappoTrainer

REFERENCE = Path(__file__).with_name("update_stats_reference.json")
KINDS, TARGETS, SLOTS = 3, 4, 4
OWN = 40
BLOCK = KINDS + TARGETS + 2 + 3 + 2 * (KINDS + TARGETS)
OBS = OWN + BLOCK


def scenario_stats(masked: bool | None = None) -> dict[str, float]:
    """Goals with four slots and a slow loop and hindsight relabelling, over two epochs of two minibatches: every
    per-minibatch statistic the update keeps."""
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8, 8), recurrent_size=6, goal_count=KINDS, goal_targets=TARGETS,
                         goal_every_decisions=3, slow_goal_size=5, goal_slots=SLOTS, hindsight_coef=0.5,
                         epochs=2, minibatches=2)
    trainer = MappoTrainer([(OBS, 3)], 4, config)
    if masked is not None:
        trainer._masked_stats = masked
    trainer.actor.goal_head.set_space(np.ones((KINDS, TARGETS), bool), [OWN])
    trainer._sync_rollout()
    steps, envs = 12, 4
    buffer = RolloutBuffer(steps, envs, 1, OBS, 4, 3, trainer.foresight_outputs, trainer.recurrent_size, True,
                           trainer.slow_goal_size, trainer.goal_slots)
    acting = trainer.acting_state(envs, 1)
    rng = np.random.default_rng(5)
    base = OWN + KINDS + TARGETS
    achieved_kind = base + 5 + KINDS + TARGETS
    for step in range(steps):
        obs = np.zeros((envs, 1, OBS), np.float32)
        obs[:, 0, OWN : OWN + KINDS + TARGETS] = 1.0
        obs[:, 0, :OWN] = rng.random((envs, OWN), dtype=np.float32)
        if step % 3 == 2:
            obs[:, 0, achieved_kind + 0] = 1.0
            obs[:, 0, achieved_kind + KINDS + 1 + step % 3] = 1.0
        state = rng.random((envs, 4), dtype=np.float32)
        mask = np.ones((envs, 1, 3), bool)
        layout = np.zeros((envs, 1), np.int64)
        memory = acting.memory.copy()
        actions, log_probs, values, foresight, goals = trainer.act_and_value(obs, mask, layout, state,
                                                                                 state=acting)
        buffer.add_decision(obs, state, mask, layout, actions, log_probs, values, None, foresight, memory, goals)
        buffer.add_outcome(rng.random((envs, 1), dtype=np.float32), np.zeros(envs, bool), np.zeros(envs, bool),
                           np.zeros((envs, 1), np.float32), np.zeros((envs, 1, trainer.foresight_outputs), np.float32))
    buffer.finish(np.zeros((envs, 1), np.float32), 0.99, 0.95,
                  slow_goal=(config.slow_goal_gamma, config.slow_goal_lambda))
    stats = trainer.update(buffer)
    return {name: float(value) for name, value in stats.items() if not name.endswith("_seconds")}


def write_reference() -> None:
    REFERENCE.write_text(json.dumps(scenario_stats(), indent=2, sort_keys=True) + "\n")


@pytest.mark.parametrize("masked", [False, True], ids=["picked_rows", "masked_rows"])
def test_the_update_reports_what_it_reported_before(masked):
    """Both ways of taking the optional terms -- picked rows with early-outs, and masked arithmetic over every row (the
    GPU's) -- report what the update did before, on the CPU: masked arithmetic is the same on any device."""
    expected = json.loads(REFERENCE.read_text())
    got = scenario_stats(masked)
    assert sorted(got) == sorted(expected)
    for name, value in expected.items():
        assert got[name] == pytest.approx(value, rel=1e-4, abs=1e-6), name
