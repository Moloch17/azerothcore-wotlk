"""M3 interact (dungeon-curriculum M3; the sim's InteractEncounter): the stage config loads, reads columns the sim
reports, steps its ladder on its gate alone with every price in full from the first step, and seeds from M2 by name;
the sight block's named row (revision 2) is read with the entity list's own encoder, kept from the adapters, matched
against the slots, and starts at zero when seeded from a checkpoint without it, so the seeded policy acts as it did."""

import re
from pathlib import Path

import numpy as np
import pytest
import torch

import test_sight as ts
import test_vision_encoder as ve
from animus.bootstrap import seed_trainer
from animus.config import TrainConfig
from animus.episode_means import PER_EVENT, means
from animus.mappo.networks import vision_of
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.train import init_from_checkpoint

CONFIGS = Path(__file__).resolve().parents[1] / "configs"
CURRICULUM = Path(__file__).resolve().parents[4] / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum"

# The named row as the sim writes it (SightBlock.h NamedTask): the entity list's 20 columns, then three tasks.
TASKS = ("reach", "interact", "use_item")
NAMED = 20 + len(TASKS)


def sim_columns() -> set[str]:
    """Every episode info column M3's sim can report: the scenario's own (its act_refused_<why> among them),
    InteractEncounter's (its per-rung columns spelled out) and the reward terms'."""
    names = set(re.findall(r'_info\.Add\("(\w+)"', (CURRICULUM / "StageScenario.cpp").read_text()))
    encounter = (CURRICULUM / "Encounters" / "InteractEncounter.cpp").read_text()
    names |= set(re.findall(r'table\.Add\("(\w+)"', encounter))
    rungs = re.search(r"RUNG_NAMES = \{ ([^}]*) \}", (CURRICULUM / "Encounters" / "InteractDraw.h").read_text())
    for rung in re.findall(r'"(\w+)"', rungs.group(1)):
        names |= {f"rung_{rung}", f"right_{rung}"}
    refusals = (CURRICULUM / "Character" / "EntityActions.cpp").read_text()
    names |= {"act_refused_" + name for name in re.findall(r'case Refusal::\w+:\s+return "(\w+)";', refusals)}
    names |= {"reward_" + name for name in re.findall(r'return "(\w+)";',
                                                       (CURRICULUM / "Rewards" / "CombatReward.cpp").read_text())}
    return names


def config() -> TrainConfig:
    return TrainConfig.load(CONFIGS / "move3_interact.yaml")


def test_the_config_loads_and_reads_its_own_measures():
    c = config()
    assert c.run_name == "move3_interact"
    assert c.convergence.measure == "right_object" and c.layout_sampling.metric == "right_object"
    headline = c.status.headline
    assert headline[0] == "right_object"
    assert {"right_distinguish", "right_switch", "right_key", "interact_rung", "door_by_lever", "key_used",
            "wrong_objects"} <= set(headline)
    # The plan's development target for M3 -- a readout, never a pass gate.
    assert c.status.targets["right_object"] == ">= 0.9"
    assert set(c.status.targets) <= set(headline)
    # None of M2's seek targets is left over.
    assert not {"found", "found_deepest", "find_seconds", "rooms_before_found"} & set(c.status.targets)
    # Every column the headline and the evaluation name is one the sim reports.
    known = sim_columns()
    missing = [name for name in (*headline, *c.eval.report) if name not in known]
    assert not missing, missing
    # A fixed evaluation at the training rung, and the held-out sweep over every rung at the stage's end.
    assert c.eval.every_env_steps == 10_000_000 and c.eval.episodes > 0
    assert c.eval.heldout == {"sweep": 60} and not c.eval.heldout_on_best
    # M2's horizon and BPTT carry.
    assert c.mappo.gamma == pytest.approx(0.998) and c.mappo.chunk_length >= c.rollout_length


def test_the_ladder_steps_on_its_gate_alone_and_every_price_is_paid_from_the_start():
    """The dungeon plan's rules, said out loud in the stage's own config: the fade steps as soon as its gate is met
    (no plateau wait), on the stage's own measure, over the sim's rungs; no cost ladder holds a price back."""
    c = config()
    assert c.fade.enabled
    assert c.fade.require_plateau is False
    assert c.fade.gate_metric == "right_object" and c.fade.gate_value == pytest.approx(0.8)
    assert c.costs.enabled is False
    text = (CONFIGS / "move3_interact.yaml").read_text()
    assert re.search(r"^fade:\n(?:  .*\n)*?  require_plateau: false$", text, re.M)
    assert re.search(r"^costs:\n(?:  .*\n)*?  enabled: false$", text, re.M)
    # The fade's rungs are InteractDraw::FADE_SCALES: a rung the sim does not know would be a rung between two.
    draw = (CURRICULUM / "Encounters" / "InteractDraw.h").read_text()
    scales = re.search(r"FADE_SCALES = \{ ([^}]*) \}", draw).group(1)
    assert tuple(c.fade.rungs) == tuple(float(v.strip().rstrip("f")) for v in scales.split(","))


def test_the_rates_by_rung_are_averaged_over_their_own_episodes():
    names = ["right_object", "rung_switch", "right_switch", "right_seconds"]
    values = np.array([[1, 1, 1, 30.0], [0, 1, 0, 0.0], [1, 0, 0, 10.0], [0, 0, 0, 0.0]])
    out = dict(zip(names, means(values, names)))
    assert PER_EVENT["right_switch"] == "rung_switch" and PER_EVENT["right_seconds"] == "right_object"
    assert out["right_switch"] == pytest.approx(0.5)
    assert out["right_seconds"] == pytest.approx(20.0)


def test_m3_seeds_from_m2_by_name(tmp_path):
    """The run's seed is the stage chain's first by name (stage.json seed_chain, the sim's Extends: move2_seek), its
    latest checkpoint (seed_from: latest)."""
    c = config()
    assert c.seed_from == "latest"
    stage = {"seed_chain": ["move2_seek", "move1_controls"]}
    chain = c.resolved_init_from(stage)
    assert chain and Path(chain[0]).parent.name == "move2_seek"
    run = tmp_path / "move2_seek"
    run.mkdir()
    (run / "latest.pt").write_text("m2")
    assert init_from_checkpoint(str(run / "best.pt"), c.seed_from) == run / "latest.pt"
    stages = (CURRICULUM / "Stages" / "Stages.cpp").read_text()
    assert re.search(r'\.Name = "move3_interact",\s*\.Suffix = "_interact",\s*\.Extends = "move2_seek"', stages)


# ------------------------------------------------------------------ the named row


def named_stage(with_sight: bool = True) -> dict:
    """test_sight's stage with the sight block's named row after its slots (revision 2)."""
    stage = ts.sight_stage(with_sight)
    if not with_sight:
        return stage
    for name in ("warrior", "priest"):
        entry = stage["layouts"][name]
        sight, goal = entry["blocks"][-2], entry["blocks"][-1]
        sight["obs"][1] += NAMED
        sight["revision"] = 2
        sight["sight"]["named"] = {"offset": ts.SLOTS * ts.WIDTH, "width": NAMED, "entity_width": 20,
                                   "tasks": list(TASKS)}
        goal["obs"][0] += NAMED
        entry["obs_dim"] += NAMED
    return stage


def named_first(name: str) -> int:
    return ts.SIGHT_FIRST[name] + ts.SLOTS * ts.WIDTH


def named_rows(count: int, seed: int = 0, entry: float = 179972.0, task: int = 0):
    """test_sight's rows with each camera row's named row: a game object of class 22 and template `entry`, task
    `task` (0 reach)."""
    obs, layout, image = ts.rows(count, seed)
    wide = torch.zeros(count, obs.shape[1] + NAMED)
    for row in range(count):
        name = ve.NAMES[int(layout[row])]
        if name not in ts.SIGHT_FIRST:
            wide[row, : obs.shape[1]] = obs[row]
            continue
        at = named_first(name)
        wide[row, :at] = obs[row, :at]
        wide[row, at + NAMED:] = obs[row, at:]
        wide[row, at + 0] = 1.0
        wide[row, at + 1] = 22.0
        wide[row, at + 2] = entry
        wide[row, at + 3] = 1.0
        wide[row, at + 20 + task] = 1.0
    return wide, layout, image


def shapes(stage: dict) -> list[tuple[int, int]]:
    return ts.shapes(stage)


def named_actor(seed: int = 0):
    torch.manual_seed(seed)
    s = named_stage()
    return ve.LayoutActor(shapes(s), [16, 16], vision=vision_of(s, ve.NAMES), recurrent_size=16)


def test_vision_of_reads_the_named_row_and_refuses_one_out_of_place():
    vision = vision_of(named_stage(), ve.NAMES)
    described = vision[0]["sight"]
    assert (described["named_offset"], described["named_width"], described["named_entity_width"]) == \
        (ts.SLOTS * ts.WIDTH, NAMED, 20)
    # Revision 1 (no row) still reads.
    assert vision_of(ts.sight_stage(), ve.NAMES)[0]["sight"]["named_width"] == 0
    wrong = named_stage()
    wrong["layouts"]["warrior"]["blocks"][-2]["sight"]["named"]["offset"] = 3
    with pytest.raises(ValueError, match="does not follow its slots"):
        vision_of(wrong, ve.NAMES)


def test_the_named_row_is_the_lists_own_token_kept_from_the_adapters_and_pooled():
    net = named_actor()
    sight = net.vision.sight
    assert sight.named_width == NAMED
    own = {key.split(".")[2] for key in net.state_dict() if key.startswith("vision.sight.")}
    assert {"named_task", "named_query", "named_pool", "named_gain"} <= own
    for index, name in enumerate(("warrior", "priest")):
        span = set(range(named_first(name), named_first(name) + NAMED))
        assert span <= set(net.vision.blind[index])
        assert bool((net.adapters[index].weight[:, sorted(span)] == 0).all())

    obs, layout, image = named_rows(6)
    token, present = sight.named(obs, layout)
    assert token.shape == (6, 64)
    assert bool(present[layout != 2].all()) and not bool(present[layout == 2].any())
    # The named token: the list's encoder over its first 20 columns, plus the task's projection.
    row = int(torch.nonzero(layout == 0)[0])
    raw = obs[row, named_first("warrior"): named_first("warrior") + NAMED]
    torch.testing.assert_close(token[row], sight.shared.token(raw[None, :20])[0] + sight.named_task(raw[20:]))
    # What it names changes the camera's embedding (the pool reads it) ...
    other, _, _ = named_rows(6, entry=2039.0)
    with torch.no_grad():
        a = net.vision(obs, layout, image)
        b = net.vision(other, layout, image)
    assert not torch.allclose(a[layout == 0], b[layout == 0])
    torch.testing.assert_close(a[layout == 2], b[layout == 2])


def test_the_presses_point_at_the_slot_matching_what_is_named():
    """Each press's scores gain the slots' match with the named token times its learned weight (0 at first): with the
    weight up, a slot of the named kind outscores the same slot holding another kind."""
    net = named_actor()
    sight = net.vision.sight
    obs, layout, image = named_rows(4)
    row = int(torch.nonzero(layout == 0)[0])
    mask = torch.ones(4, ts.ACTIONS)
    with torch.no_grad():
        features = net.features(obs, layout, image=image)
        before = net.action_logits(features, layout, mask, obs=obs)
        sight.named_gain.fill_(1.0)
        after = net.action_logits(features, layout, mask, obs=obs)
        codes, _ = sight.tokens(obs, layout)
        token, _ = sight.named(obs, layout)
        match = sight.match(codes, token)
    first = ts.BASE_ACTIONS + ts.PRESSES.index("interact") * ts.SLOTS
    # Relative to one slot (the logits share their normalisation): the gain adds the match.
    delta = (after[row, first: first + ts.SLOTS] - after[row, first]) - \
        (before[row, first: first + ts.SLOTS] - before[row, first])
    torch.testing.assert_close(delta, match[row] - match[row, 0], rtol=1e-4, atol=1e-4)
    # The gain and the match's query learn from the presses.
    net.zero_grad()
    net(obs, layout, mask, image=image).logits[:, ts.BASE_ACTIONS:].sum().backward()
    assert sight.named_gain.grad is not None and bool((sight.named_gain.grad != 0).any())
    assert sight.named_query.weight.grad is not None and bool((sight.named_query.weight.grad != 0).any())


def test_seeding_m3_from_m2_starts_the_named_row_at_zero():
    """M3 seeds from M2 (no sight block): the camera carries; the sight list and its named row start fresh, their
    pools at zero, so whatever the list holds and whatever the goal names, the seeded policy's own actions are what
    they were."""
    cfg = MappoConfig(hidden=(16, 16))
    old_stage, new_stage = named_stage(False), named_stage()
    torch.manual_seed(0)
    m2 = MappoTrainer(shapes(old_stage), 4, cfg, vision=vision_of(old_stage, ve.NAMES))
    with torch.no_grad():
        m2.actor.vision.embed.bias.normal_()
        m2.actor.vision_join.linear.weight.normal_()
    m3 = MappoTrainer(shapes(new_stage), 4, cfg, vision=vision_of(new_stage, ve.NAMES))
    seed_trainer(m3, ts.checkpoint_of(m2, old_stage), ts.spec_of(new_stage), new_stage)
    torch.testing.assert_close(m3.actor.vision.embed.bias, m2.actor.vision.embed.bias)
    sight = m3.actor.vision.sight
    for pool in (sight.pool, sight.named_pool):
        assert bool((pool.weight == 0).all()) and bool((pool.bias == 0).all())
    assert bool((sight.named_gain == 0).all())

    obs, layout, image = named_rows(6)
    other, _, _ = named_rows(6, entry=2039.0, task=2)
    for row in range(6):
        name = ve.NAMES[int(layout[row])]
        if name in ts.SIGHT_FIRST:
            other[row, ts.SIGHT_FIRST[name]: ts.SIGHT_FIRST[name] + ts.SLOTS * ts.WIDTH] = \
                torch.randn(ts.SLOTS * ts.WIDTH)
    mask = torch.ones(6, ts.ACTIONS)
    with torch.no_grad():
        before = m3.actor(obs, layout, mask, image=image).logits[:, :ts.BASE_ACTIONS]
        after = m3.actor(other, layout, mask, image=image).logits[:, :ts.BASE_ACTIONS]
    torch.testing.assert_close(before - before[:, :1], after - after[:, :1])
