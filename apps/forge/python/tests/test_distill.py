"""Merge seeding and distillation from parent stages."""

from types import SimpleNamespace

import pytest
import torch

from animus.bootstrap import seed_merges, seed_trainer
from animus.config import DistillConfig
from animus.distill import Distiller, auto_teachers, build_teacher
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout

ARENA_FIRST, ARENA_COUNT = 2, 3


def stage_with(layouts: dict[str, list[tuple[str, int, int]]], arenas=("duel", "pvp")) -> dict:
    entries = {}
    for name, blocks in layouts.items():
        obs = actions = 0
        spans = []
        for block, features, count in blocks:
            spans.append({"name": block, "obs": [obs, features], "actions": [actions, count]})
            obs += features
            actions += count
        entries[name] = {"obs_dim": obs, "num_actions": actions, "blocks": spans}
    return {"layouts": entries, "arenas": [{"name": arena} for arena in arenas],
            "state": {"arena_first": ARENA_FIRST, "arena_count": ARENA_COUNT}}


def checkpoint(trainer: MappoTrainer, layouts: list[Layout], stage: dict, hidden, recurrent_size: int = 0) -> dict:
    return {
        "trainer": trainer.state_dict(),
        "spec": {"scenario": "parent", "layouts": [{"name": l.name, "obs_dim": l.obs_dim, "num_actions": l.num_actions}
                                                   for l in layouts]},
        "config": {"mappo": {"hidden": list(hidden), "recurrent_size": recurrent_size}},
        "stage": stage,
    }


def spec(layouts: list[Layout]) -> SimpleNamespace:
    return SimpleNamespace(layouts=tuple(layouts))


def test_merge_seeds_only_blocks_the_base_lacks():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8, 8))
    pve = stage_with({"warrior_dps": [("core", 3, 2), ("duel", 2, 1), ("pack", 4, 3)]})
    pvp = stage_with({"warrior_dps": [("core", 3, 2), ("duel", 2, 1), ("pvp", 2, 0)]})
    merge = stage_with({"warrior_dps": [("core", 3, 2), ("duel", 2, 1), ("pack", 4, 3), ("pvp", 2, 0)]})
    base = MappoTrainer([(9, 6)], 5, config)
    other = MappoTrainer([(7, 3)], 5, config)
    new = MappoTrainer([(11, 6)], 5, config)
    layouts = [Layout("warrior_dps", 11, 6)]
    base_cp = checkpoint(base, [Layout("warrior_dps", 9, 6)], pve, config.hidden)
    other_cp = checkpoint(other, [Layout("warrior_dps", 7, 3)], pvp, config.hidden)

    seed_trainer(new, base_cp, spec(layouts), merge)
    assert seed_merges(new, [other_cp], spec(layouts), merge, base_cp) == {"warrior_dps": ["pvp"]}

    for network, base_net, other_net in ((new.actor, base.actor, other.actor), (new.critic, base.critic, other.critic)):
        w = network.state_dict()["adapters.0.weight"]
        torch.testing.assert_close(w[:, 0:9], base_net.state_dict()["adapters.0.weight"])     # core, duel, pack
        torch.testing.assert_close(w[:, 9:11], other_net.state_dict()["adapters.0.weight"][:, 5:7])  # pvp
    # The trunk is the base's; the merged stage only brought the pvp columns.
    torch.testing.assert_close(new.actor.state_dict()["trunk.layers.0.weight"],
                               base.actor.state_dict()["trunk.layers.0.weight"])


def test_trainer_update_adds_the_auxiliary_loss():
    torch.manual_seed(0)
    from animus.mappo.buffer import RolloutBuffer

    config = MappoConfig(hidden=(8,), epochs=1, minibatches=1)
    trainer = MappoTrainer([(3, 2)], 5, config)
    buffer = RolloutBuffer(4, 2, 1, 3, 5, 2)
    for _ in range(4):
        obs = torch.randn(2, 1, 3).numpy()
        state = torch.zeros(2, 5).numpy()
        mask = torch.ones(2, 1, 2, dtype=torch.bool).numpy()
        layout = torch.zeros(2, 1, dtype=torch.long).numpy()
        actions, log_probs = trainer.act(obs, mask, layout)
        buffer.add_decision(obs, state, mask, layout, actions, log_probs, trainer.value(state, obs, layout))
        buffer.add_outcome(torch.ones(2, 1).numpy(), torch.zeros(2, dtype=torch.bool).numpy(),
                           torch.zeros(2, dtype=torch.bool).numpy(), torch.zeros(2, 1).numpy())
    buffer.finish(torch.zeros(2, 1).numpy(), 0.99, 0.95)

    calls = []

    def auxiliary(data, idx, dist):
        calls.append(len(idx))
        return dist.logits.sum() * 0.0, {"distill_kl": 0.5}

    stats = trainer.update(buffer, auxiliary)
    assert calls == [8] and stats["distill_kl"] == pytest.approx(0.5)
    assert "distill_kl" not in trainer.update(buffer)


def test_teacher_equal_to_the_policy_has_zero_kl_and_block_moves_are_mapped():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(16, 16))
    parent_stage = stage_with({"mage_dps": [("core", 4, 3), ("pvp", 2, 0)]}, arenas=("pvp",))
    # The merge puts a pack block between core and pvp: pvp's columns move.
    stage = stage_with({"mage_dps": [("core", 4, 3), ("pack", 3, 2), ("pvp", 2, 0)]})
    parent = MappoTrainer([(6, 3)], 5, config)
    student = MappoTrainer([(9, 5)], 5, config)
    parent_cp = checkpoint(parent, [Layout("mage_dps", 6, 3)], parent_stage, config.hidden)
    layouts = [Layout("mage_dps", 9, 5)]
    seed_trainer(student, parent_cp, spec(layouts), stage)

    teacher = build_teacher(parent_cp, spec(layouts), stage, torch.device("cpu"))
    mapping = teacher.layouts[0]
    assert mapping.obs_student.tolist() == [0, 1, 2, 3, 7, 8] and mapping.obs_teacher.tolist() == [0, 1, 2, 3, 4, 5]
    assert mapping.actions_student.tolist() == [0, 1, 2] and mapping.actions_teacher.tolist() == [0, 1, 2]

    distiller = Distiller(stage, {"pvp": teacher})
    rows = 6
    obs = torch.randn(rows, 9)
    obs[:, 4:7] = 0.0  # the new pack block starts at zero weight, but keep it quiet anyway
    state = torch.zeros(rows, 5)
    state[:3, ARENA_FIRST + 1] = 1.0  # pvp: taught
    state[3:, ARENA_FIRST + 0] = 1.0  # duel: no teacher
    layout = torch.zeros(rows, dtype=torch.long)
    mask = torch.ones(rows, 5, dtype=torch.bool)
    log_probs = student.actor(obs, layout, mask).logits

    total, taught = distiller.kl(obs, state, layout, mask, log_probs)
    assert taught == 3
    assert float(total.detach()) == pytest.approx(0.0, abs=1e-5)

    # A different policy has a positive KL, and gradients reach it.
    other = MappoTrainer([(9, 5)], 5, config)
    log_probs = other.actor(obs, layout, mask).logits
    total, taught = distiller.kl(obs, state, layout, mask, log_probs)
    assert float(total.detach()) > 1e-4
    total.backward()
    assert other.actor.heads[0].weight.grad is not None


def test_masked_actions_are_left_out():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(8,))
    stage = stage_with({"rogue_dps": [("core", 3, 3)]})
    parent = MappoTrainer([(3, 3)], 5, config)
    layouts = [Layout("rogue_dps", 3, 3)]
    teacher = build_teacher(checkpoint(parent, layouts, stage, config.hidden), spec(layouts), stage,
                            torch.device("cpu"))
    distiller = Distiller(stage, {"duel": teacher})

    obs = torch.randn(2, 3)
    state = torch.zeros(2, 5)
    state[:, ARENA_FIRST] = 1.0
    layout = torch.zeros(2, dtype=torch.long)
    mask = torch.tensor([[True, False, False], [False, False, False]])
    # One allowed action: both distributions put everything on it, so nothing to learn; no allowed action: skipped.
    total, taught = distiller.kl(obs, state, layout, mask, torch.log_softmax(torch.randn(2, 3), dim=-1))
    assert taught == 1 and float(total) == pytest.approx(0.0, abs=1e-6)


def test_auto_teachers_take_the_first_parent_with_the_arena():
    stage = {"arenas": [{"name": "duel"}, {"name": "pvp"}, {"name": "ambush"}]}
    base = {"stage": {"arenas": [{"name": "pvp"}]}}
    merged = {"stage": {"arenas": [{"name": "duel"}, {"name": "pvp"}]}}
    chosen = auto_teachers(stage, [base, merged])
    assert chosen["pvp"] is base and chosen["duel"] is merged and "ambush" not in chosen


def test_distiller_rejects_unknown_arenas_and_missing_state_span():
    stage = stage_with({})
    with pytest.raises(ValueError, match="no arena"):
        Distiller(stage, {"raid": object()})
    with pytest.raises(ValueError, match="arena state span"):
        Distiller({"arenas": [{"name": "duel"}]}, {})


def test_coefficient_decays_to_its_floor():
    config = DistillConfig(coef=2.0, half_life_env_steps=10, min_coef=0.1)
    assert config.coef_at(0) == pytest.approx(2.0)
    assert config.coef_at(10) == pytest.approx(1.0)
    assert config.coef_at(1000) == pytest.approx(0.1)


def test_a_chunk_at_once_matches_replaying_it_decision_by_decision():
    """sequence_loss is the batched form of the per-decision path: same teachers, same memories, same KL. The loop it
    replaces cost stage27_crossroads a 306 s update with six teachers."""
    torch.manual_seed(0)
    config = MappoConfig(hidden=(16, 16), recurrent_size=4)
    parent_stage = stage_with({"mage_dps": [("core", 4, 3), ("pvp", 2, 0)]}, arenas=("pvp",))
    stage = stage_with({"mage_dps": [("core", 4, 3), ("pvp", 2, 0)]})
    parent = MappoTrainer([(6, 3)], 5, config)
    student = MappoTrainer([(6, 3)], 5, config)
    teacher = build_teacher(
        checkpoint(parent, [Layout("mage_dps", 6, 3)], parent_stage, config.hidden, config.recurrent_size),
        spec([Layout("mage_dps", 6, 3)]), stage, torch.device("cpu"))
    assert teacher.recurrent_size  # the path under test is the recurrent one

    steps, rows = 5, 4
    obs = torch.randn(steps, rows, 6)
    state = torch.zeros(steps, rows, 5)
    state[:, :2, ARENA_FIRST + 1] = 1.0          # pvp: taught
    state[:, 2:, ARENA_FIRST + 0] = 1.0          # duel: no teacher, but still replayed
    layout = torch.zeros(steps, rows, dtype=torch.long)
    mask = torch.ones(steps, rows, 3, dtype=torch.bool)
    dones = torch.zeros(steps, rows, dtype=torch.bool)
    dones[2, 1] = True                            # an episode ends mid-chunk: the memory clears there
    logits = torch.stack([student.actor(obs[step], layout[step], mask[step]).logits for step in range(steps)])

    distiller = Distiller(stage, {"pvp": teacher})
    distiller.coef = 0.5          # the trainer sets this per update; at 0 nothing is taught either way
    memories = distiller.begin_sequence(rows, torch.device("cpu"))
    stepwise_total = 0.0
    stepwise_rows = 0
    for step in range(steps):
        taught = distiller.step_loss(obs[step], state[step], layout[step], mask[step], logits[step], memories,
                                     dones[step])
        if taught is not None:
            loss, count = taught
            stepwise_total += float(loss.detach()) * count
            stepwise_rows += count

    batched = distiller.sequence_loss(obs, state, layout, mask, logits, dones)
    assert batched is not None
    loss, count = batched
    assert count == stepwise_rows
    assert float(loss.detach()) == pytest.approx(stepwise_total / stepwise_rows, rel=1e-5)
