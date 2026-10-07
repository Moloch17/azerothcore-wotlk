"""The combat stages (dungeon-curriculum C1-C3; the sim's CombatEncounter) and their perception-true inputs (I3): the
configs load and read their own measures, every ladder steps on its gate alone with every price at full price from the
start, the seed chain runs from M2, and a sight list widened by the combat columns seeds from a narrower one with
the seeded policy acting as it did."""

import re
from pathlib import Path
from types import SimpleNamespace

import pytest
import torch

import test_vision_encoder as ve
from animus.bootstrap import seed_trainer
from animus.config import TrainConfig
from animus.episode_means import PER_EVENT
from animus.mappo.networks import vision_of
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout

CONFIGS = Path(__file__).resolve().parents[1] / "configs"
CURRICULUM = Path(__file__).resolve().parents[4] / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum"
STAGES = ("combat1_fight", "combat2_packs", "combat3_survive")


def sim_columns() -> set[str]:
    """Every episode info column a combat stage's sim can report: the scenario's own, CombatEncounter's, the reward
    terms'."""
    names = set(re.findall(r'_info\.Add\("(\w+)"', (CURRICULUM / "StageScenario.cpp").read_text()))
    names |= set(re.findall(r'table\.Add\("(\w+)"', (CURRICULUM / "Encounters" / "CombatEncounter.cpp").read_text()))
    names |= {"reward_" + name for name in re.findall(r'return "(\w+)";',
                                                       (CURRICULUM / "Rewards" / "CombatReward.cpp").read_text())}
    return names


def stage_extends() -> dict[str, str]:
    text = (CURRICULUM / "Stages" / "Stages.cpp").read_text()
    return dict(re.findall(r'\.Name = "(\w+)",\s*\.Suffix = "\w+",\s*\.Extends = "(\w*)"', text))


@pytest.mark.parametrize("name", STAGES)
def test_the_config_loads_and_reads_its_own_measures(name):
    config = TrainConfig.load(CONFIGS / f"{name}.yaml")
    assert config.run_name == name
    known = sim_columns()
    missing = [column for column in (*config.status.headline, *config.eval.report) if column not in known]
    assert not missing, missing
    assert set(config.status.targets) <= set(config.status.headline)
    # The rung the evaluation videos read (Vision::EvalVideoRungColumn: a name ending _rung, never at_top_rung), and the
    # outcome they call a success by (EVAL_VIDEO_OUTCOMES: "won" before the rest).
    assert "combat_rung" in config.eval.report and "won" in config.eval.report
    assert config.convergence.measure in ("won", "survived")
    assert config.layout_sampling.metric == config.convergence.measure


@pytest.mark.parametrize("name", STAGES)
def test_every_ladder_steps_on_its_gate_alone_with_full_prices_from_the_start(name):
    """The dungeon plan's rules (the coordinator's checklist): the cost ladder off -- every price at full price from the
    first step, so nothing waits on it -- and the shaping fade stepping on its gate at the first evaluation that meets
    it, with no plateau and no wait for the classes' own rungs (DifficultyLadder steps on its window alone)."""
    config = TrainConfig.load(CONFIGS / f"{name}.yaml")
    assert config.costs.enabled is False
    assert config.fade.enabled is True
    assert config.fade.require_plateau is False
    assert config.fade.gate_metric in config.eval.report
    assert config.fade.gate_metric in config.status.headline
    assert 0.0 < config.fade.gate_value <= 1.0
    assert config.fade.rungs[0] == 1.0 and config.fade.rungs[-1] == 0.0
    assert config.fade.moving_classes >= 100


def test_the_seed_chain_runs_from_m2():
    """C1 extends move3_interact (the plan's base), C2 C1, C3 C2; each config seeds from
    its stage's chain (init_from auto, the parent's latest.pt first)."""
    extends = stage_extends()
    assert extends["combat1_fight"] == "move3_interact"
    assert extends["combat2_packs"] == "combat1_fight"
    assert extends["combat3_survive"] == "combat2_packs"
    for name in STAGES:
        config = TrainConfig.load(CONFIGS / f"{name}.yaml")
        assert config.init_from == "auto" and config.seed_from == "latest"
    config = TrainConfig.load(CONFIGS / "combat1_fight.yaml")
    chain = config.resolved_init_from({"seed_chain": ["move3_interact", "move2_seek", "move1_controls"]})
    assert [Path(path).parent.name for path in chain] == ["move3_interact", "move2_seek", "move1_controls"]


def test_survive_is_paid_for_staying_alive_and_never_for_coming_back():
    """C3's purpose is Survived; the walk back after a death is priced (Away), never paid (a Rejoin outcome would pay
    dying and coming back)."""
    config = TrainConfig.load(CONFIGS / "combat3_survive.yaml")
    assert config.convergence.measure == "survived" and config.status.headline[0] == "survived"
    assert "reward_away" in config.eval.report and "reward_rejoin" not in config.eval.report
    assert {"rejoined", "dead_seconds", "away_seconds"} <= set(config.status.headline)
    assert "Rejoin" not in (CURRICULUM / "Rewards" / "RewardLedger.h").read_text().split("enum class RewardCategory")[0]


def test_the_per_event_measures():
    assert PER_EVENT["rejoin_seconds"] == "rejoins"
    assert PER_EVENT["kill_seconds"] == "kills"
    assert PER_EVENT["interrupt_earnings"] == "outcome_paid"


def test_c2_watches_the_interrupts_earnings():
    """InterruptLanded against Kill and Clear, read as the ratio of the sums (each episode's ratio weighted by what its
    kills and clears earned), in C2's evaluation and headline, its readout at 0.3."""
    config = TrainConfig.load(CONFIGS / "combat2_packs.yaml")
    assert {"interrupt_earnings", "outcome_paid"} <= set(config.eval.report)
    assert "interrupt_earnings" in config.status.headline
    assert config.status.targets["interrupt_earnings"] == "<= 0.3"
    import numpy as np
    from animus.episode_means import means
    names = ["interrupt_earnings", "outcome_paid"]
    rows = np.array([[0.5, 2.0], [0.0, 6.0], [0.0, 0.0]])
    assert means(rows, names)[0] == pytest.approx(1.0 / 8.0)


# ------------------------------------------------------------------ the sight list, widened by the combat columns

PRESSES = ("select", "interact", "use_item", "assist", "focus")
BASE_ACTIONS = 5
VISIBLE, RECALLED = ve.SLOTS, 2
SLOTS = VISIBLE + RECALLED
NARROW, COMBAT_COLUMNS = 32, 12
WIDE = NARROW + COMBAT_COLUMNS
ACTIONS = BASE_ACTIONS + len(PRESSES) * SLOTS
EXTRA = ["visible", "age", "dead", "open", "used", "heading_sin", "heading_cos", "speed", "course_sin", "course_cos",
         "selected", "focused"]
COMBAT = ["combat_casting", "combat_cast_left", "combat_interruptible", "combat_cast_heal", "combat_cast_area",
          "combat_cast_at_me", "combat_controlled", "combat_elite", "combat_in_combat", "combat_attacks_me",
          "combat_attacks_party", "combat_threat"]


def sight_stage(width: int) -> dict:
    """test_vision_encoder's stage with a sight block of `width` columns a slot after the entity list (32: M3's; 44: a
    layout with the combat block)."""
    stage = ve.stage()
    for name in ("warrior", "priest"):
        entry = stage["layouts"][name]
        goal = entry["blocks"].pop()
        at = goal["obs"][0]
        features = ve.ENTITY_NAMES + EXTRA + (COMBAT if width == WIDE else [])
        sight = {"name": "sight", "slots": SLOTS, "visible_slots": VISIBLE, "recalled_slots": RECALLED,
                 "width": width, "first": at, "present": 0, "class_column": 1, "type_column": 2, "object_column": 3,
                 "memory_column": 19, "visible_column": 20, "classes": 32, "type_buckets": 64, "memory_ids": 64,
                 "features": features,
                 "pointers": [{"press": press, "first": BASE_ACTIONS + i * SLOTS, "count": SLOTS}
                              for i, press in enumerate(PRESSES)]}
        entry["blocks"].append(ve.block("sight", at, SLOTS * width, (BASE_ACTIONS, len(PRESSES) * SLOTS), revision=1,
                                        sight=sight))
        at += SLOTS * width
        entry["blocks"].append(ve.block("goal", at, 2))
        entry["obs_dim"] = at + 2
    return stage


def shapes(stage: dict) -> list[tuple[int, int]]:
    return [(stage["layouts"][name]["obs_dim"], ACTIONS if name != "director" else 4) for name in ve.NAMES]


def spec_of(stage: dict) -> SimpleNamespace:
    return SimpleNamespace(layouts=tuple(Layout(name, obs, actions)
                                         for name, (obs, actions) in zip(ve.NAMES, shapes(stage))), state_dim=4)


def checkpoint_of(trainer: MappoTrainer, stage: dict) -> dict:
    return {"trainer": trainer.state_dict(), "stage": stage,
            "spec": {"layouts": [{"name": l.name, "obs_dim": l.obs_dim, "num_actions": l.num_actions}
                                 for l in spec_of(stage).layouts]}}


def test_a_sight_list_widened_by_the_combat_columns_seeds_from_a_narrower_one():
    """C1 after M3 (whose layout has the sight list without the combat block): the list's encoder and pointer queries
    carry, its projection of a slot's extra columns keeps M3's weights for the memory's columns and starts the combat
    columns at zero -- so whatever the combat columns hold, the seeded policy's logits are M3's."""
    config = MappoConfig(hidden=(16, 16))
    narrow, wide = sight_stage(NARROW), sight_stage(WIDE)
    torch.manual_seed(0)
    m3 = MappoTrainer(shapes(narrow), 4, config, vision=vision_of(narrow, ve.NAMES))
    with torch.no_grad():
        m3.actor.vision.sight.extra.weight.normal_()
        m3.actor.vision.sight.pool.weight.normal_()
        m3.actor.sight_pointers.queries["select"].weight.normal_()
    c1 = MappoTrainer(shapes(wide), 4, config, vision=vision_of(wide, ve.NAMES))
    seed_trainer(c1, checkpoint_of(m3, narrow), spec_of(wide), wide)

    before, after = m3.actor.vision.sight.extra.weight, c1.actor.vision.sight.extra.weight
    assert after.shape[1] == before.shape[1] + COMBAT_COLUMNS
    torch.testing.assert_close(after[:, : before.shape[1]], before)
    assert bool((after[:, before.shape[1]:] == 0).all())
    torch.testing.assert_close(c1.actor.vision.sight.pool.weight, m3.actor.vision.sight.pool.weight)
    torch.testing.assert_close(c1.actor.sight_pointers.queries["select"].weight,
                               m3.actor.sight_pointers.queries["select"].weight)

    # The same rows, the combat columns filled at random: the seeded tokens read them as nothing.
    rows = 4
    layout = torch.tensor([0, 1, 0, 1])
    width = max(wide["layouts"][name]["obs_dim"] for name in ve.NAMES)
    obs = torch.zeros(rows, width)
    generator = torch.Generator().manual_seed(3)
    for row in range(rows):
        name = ve.NAMES[int(layout[row])]
        first = next(b for b in wide["layouts"][name]["blocks"] if b["name"] == "sight")["obs"][0]
        block = torch.randn(SLOTS, WIDE, generator=generator)
        block[:, 0] = 1.0
        block[:, 1] = torch.randint(0, 23, (SLOTS,), generator=generator).float()
        block[:, 2] = torch.randint(0, 5000, (SLOTS,), generator=generator).float()
        block[:, 3] = 0.0
        block[:, 19] = torch.randint(0, 64, (SLOTS,), generator=generator).float()
        obs[row, first: first + SLOTS * WIDE] = block.reshape(-1)
    other = obs.clone()
    for row in range(rows):
        name = ve.NAMES[int(layout[row])]
        first = next(b for b in wide["layouts"][name]["blocks"] if b["name"] == "sight")["obs"][0]
        for slot in range(SLOTS):
            at = first + slot * WIDE + NARROW
            other[row, at: at + COMBAT_COLUMNS] = torch.randn(COMBAT_COLUMNS, generator=generator)
    with torch.no_grad():
        codes, present = c1.actor.vision.sight.tokens(obs, layout)
        again, _ = c1.actor.vision.sight.tokens(other, layout)
    torch.testing.assert_close(codes, again)
    assert bool(present.all())


# ------------------------------------------------------------------ Sight revision 2: the named row

TASKS = ("reach", "interact", "use_item")
NAMED = 20 + len(TASKS)


def named_stage(width: int) -> dict:
    """sight_stage with the sight block's named row after its slots (revision 2), as every stage with the sight block
    now has it -- present and 0 in the combat stages, where nothing is named."""
    stage = sight_stage(width)
    for name in ("warrior", "priest"):
        entry = stage["layouts"][name]
        sight, goal = entry["blocks"][-2], entry["blocks"][-1]
        sight["obs"][1] += NAMED
        sight["revision"] = 2
        sight["sight"]["named"] = {"offset": SLOTS * width, "width": NAMED, "entity_width": 20, "tasks": list(TASKS)}
        goal["obs"][0] += NAMED
        entry["obs_dim"] += NAMED
    return stage


def test_a_c1_checkpoint_on_sight_revision_1_seeds_into_revision_2():
    """A C1 checkpoint from before M3 (sight revision 1: its slots widened by the combat columns, no named row) seeds a
    C1 on revision 2: the list's encoder, its combat columns' projection, its pool and its pointer queries carry as
    they were, so its slot columns read exactly as before; the named row starts fresh (its pool and gain at zero)."""
    config = MappoConfig(hidden=(16, 16))
    old_stage, new_stage = sight_stage(WIDE), named_stage(WIDE)
    torch.manual_seed(0)
    old = MappoTrainer(shapes(old_stage), 4, config, vision=vision_of(old_stage, ve.NAMES))
    with torch.no_grad():
        for tensor in (old.actor.vision.sight.extra.weight, old.actor.vision.sight.pool.weight,
                       old.actor.vision.sight.memory_embed.weight, old.actor.sight_pointers.queries["select"].weight):
            tensor.normal_()
    new = MappoTrainer(shapes(new_stage), 4, config, vision=vision_of(new_stage, ve.NAMES))
    seed_trainer(new, checkpoint_of(old, old_stage), spec_of(new_stage), new_stage)

    before, after = old.actor.vision.sight, new.actor.vision.sight
    for key in ("extra.weight", "extra.bias", "pool.weight", "pool.bias", "memory_embed.weight"):
        torch.testing.assert_close(after.state_dict()[key], before.state_dict()[key])
    torch.testing.assert_close(new.actor.sight_pointers.queries["select"].weight,
                               old.actor.sight_pointers.queries["select"].weight)
    assert bool((after.named_pool.weight == 0).all()) and bool((after.named_pool.bias == 0).all())
    assert bool((after.named_gain == 0).all())

    # The same slots in both layouts (the new one's named row 0, nothing named): the same tokens.
    rows = 4
    layout = torch.tensor([0, 1, 0, 1])
    generator = torch.Generator().manual_seed(5)

    def obs_of(stage: dict, blocks: list[torch.Tensor]) -> torch.Tensor:
        width = max(stage["layouts"][name]["obs_dim"] for name in ve.NAMES)
        out = torch.zeros(rows, width)
        for row in range(rows):
            name = ve.NAMES[int(layout[row])]
            first = next(b for b in stage["layouts"][name]["blocks"] if b["name"] == "sight")["obs"][0]
            out[row, first: first + SLOTS * WIDE] = blocks[row].reshape(-1)
        return out

    blocks = []
    for _ in range(rows):
        block = torch.randn(SLOTS, WIDE, generator=generator)
        block[:, 0] = 1.0
        block[:, 1] = torch.randint(0, 23, (SLOTS,), generator=generator).float()
        block[:, 2] = torch.randint(0, 5000, (SLOTS,), generator=generator).float()
        block[:, 3] = 0.0
        block[:, 19] = torch.randint(0, 64, (SLOTS,), generator=generator).float()
        blocks.append(block)
    with torch.no_grad():
        old_codes, _ = before.tokens(obs_of(old_stage, blocks), layout)
        new_codes, _ = after.tokens(obs_of(new_stage, blocks), layout)
    torch.testing.assert_close(new_codes, old_codes)
