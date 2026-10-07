"""The sight list (dungeon-curriculum I1 and I2): the sight block read beside the camera's entity list -- its visible
half and its remembered half as one set, through the list's own encoder -- its pointer heads over every slot, its
columns kept from the adapters, and seeding: a stage with it seeds from one without, the camera carried, the list
fresh with its pool at zero, the policy as it was."""

from types import SimpleNamespace

import pytest
import torch

import test_vision_encoder as ve
from animus.bootstrap import seed_trainer
from animus.mappo.networks import (SightEntities, SightPointers, VisionEncoder, vision_of)
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout

# The sight block as the sim writes it (SightBlock.h), shortened: the entity list's 3 visible slots and 2
# remembered, 32 columns a slot (the list's 20, then the memory's 12), five presses of every slot.
VISIBLE, RECALLED = ve.SLOTS, 2
SLOTS, WIDTH = VISIBLE + RECALLED, 32
PRESSES = ("select", "interact", "use_item", "assist", "focus")
BASE_ACTIONS = 5
ACTIONS = BASE_ACTIONS + len(PRESSES) * SLOTS
MEMORY_IDS = 64
EXTRA_NAMES = ["visible", "age", "dead", "open", "used", "heading_sin", "heading_cos", "speed", "course_sin",
               "course_cos", "selected", "focused"]
MEMORY, VISIBLE_COLUMN = 19, 20


def sight(first: int, actions_first: int) -> dict:
    return {"name": "sight", "slots": SLOTS, "visible_slots": VISIBLE, "recalled_slots": RECALLED, "width": WIDTH,
            "first": first, "present": 0, "class_column": 1, "type_column": 2, "object_column": 3,
            "memory_column": MEMORY, "visible_column": VISIBLE_COLUMN, "classes": 32, "type_buckets": 64,
            "memory_ids": MEMORY_IDS, "features": ve.ENTITY_NAMES + EXTRA_NAMES,
            "pointers": [{"press": press, "first": actions_first + at * SLOTS, "count": SLOTS}
                         for at, press in enumerate(PRESSES)]}


def sight_stage(with_sight: bool = True) -> dict:
    """test_vision_encoder's stage with a sight block after the entity list, before the goal."""
    stage = ve.stage()
    for name in ("warrior", "priest"):
        entry = stage["layouts"][name]
        goal = entry["blocks"].pop()
        at = goal["obs"][0]
        if with_sight:
            entry["blocks"].append(ve.block("sight", at, SLOTS * WIDTH, (BASE_ACTIONS, len(PRESSES) * SLOTS),
                                            revision=1, sight=sight(at, BASE_ACTIONS)))
            at += SLOTS * WIDTH
        entry["blocks"].append(ve.block("goal", at, 2))
        entry["obs_dim"] = at + 2
    return stage


SIGHT_FIRST = {name: ve.FIRST[name] + ve.SPAN + ve.SLOTS * ve.FEATURES for name in ("warrior", "priest")}


def shapes(stage: dict) -> list[tuple[int, int]]:
    actions = ACTIONS if any(b["name"] == "sight" for b in stage["layouts"]["warrior"]["blocks"]) else BASE_ACTIONS
    return [(stage["layouts"][name]["obs_dim"], actions if name != "director" else 4) for name in ve.NAMES]


def rows(count: int, seed: int = 0, layouts=None):
    """test_vision_encoder.observations with each camera row's sight list: some slots present, their class, type,
    object and memory id as the sim writes them. (obs, layout, image)."""
    s = sight_stage()
    obs, layout, image = ve.observations(count, seed, layouts)
    width = max(s["layouts"][name]["obs_dim"] for name in ve.NAMES)
    wide = torch.zeros(count, width)
    wide[:, : obs.shape[1]] = obs
    generator = torch.Generator().manual_seed(seed + 100)
    for row in range(count):
        name = ve.NAMES[int(layout[row])]
        if name not in SIGHT_FIRST:
            continue
        # The goal block moved past the list: its two columns follow it.
        goal = SIGHT_FIRST[name] + SLOTS * WIDTH
        wide[row, goal: goal + 2] = obs[row, SIGHT_FIRST[name]: SIGHT_FIRST[name] + 2]
        block = torch.randn(SLOTS, WIDTH, generator=generator)
        block[:, 0] = (torch.rand(SLOTS, generator=generator) < 0.7).float()
        block[:, 1] = torch.randint(0, 23, (SLOTS,), generator=generator).float()
        block[:, 2] = torch.randint(0, 5000, (SLOTS,), generator=generator).float()
        block[:, 3] = torch.randint(0, 2, (SLOTS,), generator=generator).float()
        block[:, MEMORY] = torch.randint(0, 80, (SLOTS,), generator=generator).float()
        block[:VISIBLE, VISIBLE_COLUMN] = 1.0
        block[VISIBLE:, VISIBLE_COLUMN] = 0.0
        wide[row, SIGHT_FIRST[name]: SIGHT_FIRST[name] + SLOTS * WIDTH] = block.reshape(-1)
    return wide, layout, image


def slot_columns(name: str, slot: int) -> slice:
    first = SIGHT_FIRST[name] + slot * WIDTH
    return slice(first, first + WIDTH)


def actor(seed: int = 0):
    torch.manual_seed(seed)
    s = sight_stage()
    return ve.LayoutActor(shapes(s), [16, 16], vision=vision_of(s, ve.NAMES), recurrent_size=16)


# ------------------------------------------------------------------ stage.json


def test_vision_of_reads_the_sight_block():
    vision = vision_of(sight_stage(), ve.NAMES)
    described = vision[0]["sight"]
    assert described["first"] == SIGHT_FIRST["warrior"] and vision[1]["sight"]["first"] == SIGHT_FIRST["priest"]
    assert (described["slots"], described["visible_slots"], described["width"]) == (SLOTS, VISIBLE, WIDTH)
    assert described["presses"] == PRESSES
    assert described["pointer_first"] == tuple(BASE_ACTIONS + at * SLOTS for at in range(len(PRESSES)))
    assert vision[2] is None
    # A stage without one describes none: its cameras are as they were.
    assert all("sight" not in entry for entry in vision_of(sight_stage(False), ve.NAMES) if entry is not None)


def test_vision_of_refuses_a_sight_list_it_cannot_read():
    other = sight_stage()
    other["layouts"]["warrior"]["blocks"][-2]["sight"]["visible_slots"] = VISIBLE + 1
    with pytest.raises(ValueError, match="does not lead with the entity list"):
        vision_of(other, ve.NAMES)
    short = sight_stage()
    short["layouts"]["warrior"]["blocks"][-2]["obs"][1] -= 1
    with pytest.raises(ValueError, match="the sight block spans"):
        vision_of(short, ve.NAMES)


# ------------------------------------------------------------------ the set encoding


def test_the_list_is_read_with_the_entity_lists_own_encoder():
    net = actor()
    encoder = net.vision.sight
    assert isinstance(encoder, SightEntities) and isinstance(net.vision, VisionEncoder)
    assert encoder.shared is net.vision.entities
    # Shared by reference: the sight list has no copy of the list's weights in the state dict.
    own = [key for key in net.state_dict() if key.startswith("vision.sight.")]
    assert own and all(key.split(".")[2] in ("extra", "memory_embed", "pool") for key in own), own

    obs, layout, image = rows(6)
    codes, present = encoder.tokens(obs, layout)
    assert codes.shape == (6, SLOTS, 64) and present.shape == (6, SLOTS)
    assert not bool(present[layout == 2].any())             # the director has no list

    # A slot's token: the list's encoder over its first 20 columns, the memory's columns and its id added.
    row = int(torch.nonzero(layout == 0)[0])
    raw = obs[row, slot_columns("warrior", 0)]
    expected = (encoder.shared.token(raw[None, None, :20])[0, 0] + encoder.extra(raw[20:])
                + encoder.memory_embed(torch.tensor(int(raw[MEMORY].round().clamp(min=0)) and
                                                    (int(raw[MEMORY].round()) - 1) % MEMORY_IDS + 1)))
    torch.testing.assert_close(codes[row, 0], expected)

    # The memory id is a label: the list's encoder reads it as 0; the sight list embeds it (none: the zero row).
    with_id = raw[None, None, :20].clone()
    without = with_id.clone()
    without[..., MEMORY] = 0.0
    torch.testing.assert_close(encoder.shared.token(with_id), encoder.shared.token(without))
    assert bool((encoder.memory_embed.weight[0] == 0).all())

    pooled = encoder.pooled(codes, present, layout)
    assert pooled.shape == (6, VisionEncoder.EMBED)
    assert bool((pooled[layout == 2] == 0).all())


def test_the_list_columns_are_blind_to_the_adapters():
    net = actor()
    for index, name in enumerate(("warrior", "priest")):
        span = set(range(SIGHT_FIRST[name], SIGHT_FIRST[name] + SLOTS * WIDTH))
        assert span <= set(net.vision.blind[index])
        assert bool((net.adapters[index].weight[:, sorted(span)] == 0).all())


# ------------------------------------------------------------------ the pointer heads


def test_the_pointer_heads_score_every_slot_of_the_combined_list():
    net = actor()
    assert isinstance(net.sight_pointers, SightPointers)
    assert sorted(net.sight_pointers.queries) == sorted(PRESSES)
    obs, layout, image = rows(6)
    mask = torch.ones(6, ACTIONS)
    out = net(obs, layout, mask, image=image)
    assert out.logits.shape == (6, ACTIONS)

    # A press follows its entity, not its slot: swap a warrior row's first visible and first remembered slots and
    # each press's logits swap with them.
    row = int(torch.nonzero(layout == 0)[0])
    swapped = obs.clone()
    swapped[row, slot_columns("warrior", 0)] = obs[row, slot_columns("warrior", VISIBLE)]
    swapped[row, slot_columns("warrior", VISIBLE)] = obs[row, slot_columns("warrior", 0)]
    with torch.no_grad():
        features = net.features(obs, layout, image=image)
        before = net.action_logits(features, layout, mask, obs=obs)
        after = net.action_logits(features, layout, mask, obs=swapped)
    for at in range(len(PRESSES)):
        first = BASE_ACTIONS + at * SLOTS
        torch.testing.assert_close(after[row, first], before[row, first + VISIBLE])
        torch.testing.assert_close(after[row, first + VISIBLE], before[row, first])
    # The layout's own actions are its head's, untouched by the list's content.
    torch.testing.assert_close(after[row, :BASE_ACTIONS], before[row, :BASE_ACTIONS])

    # Both the queries and the shared tokens learn from the presses.
    net.zero_grad()
    net(obs, layout, mask, image=image).logits[:, BASE_ACTIONS:].sum().backward()
    for parameter in (net.sight_pointers.queries["interact"].weight, net.vision.sight.extra.weight,
                      net.vision.entities.encoders["visible"][0].weight, net.vision.sight.memory_embed.weight):
        assert parameter.grad is not None and bool((parameter.grad != 0).any())


def test_a_masked_slot_cannot_be_drawn():
    net = actor()
    obs, layout, image = rows(4, layouts=[0, 1, 0, 1])
    mask = torch.ones(4, ACTIONS)
    mask[:, BASE_ACTIONS:] = 0
    mask[:, BASE_ACTIONS + 2] = 1
    with torch.no_grad():
        probs = net(obs, layout, mask, image=image).probs
    assert bool((probs[:, BASE_ACTIONS + 3:] == 0).all())


def test_rollout_graphs_stay_on_with_the_sight_list():
    s = sight_stage()
    trainer = MappoTrainer(shapes(s), 4, MappoConfig(hidden=(16, 16)), vision=vision_of(s, ve.NAMES))
    trainer._rollout_stream = object()
    assert trainer._graphs_apply(trainer.acting_state(2, 1))


# ------------------------------------------------------------------ seeding


def checkpoint_of(trainer: MappoTrainer, stage: dict) -> dict:
    layouts = [Layout(name, obs, actions) for name, (obs, actions) in zip(ve.NAMES, shapes(stage))]
    return {"trainer": trainer.state_dict(), "stage": stage,
            "spec": {"layouts": [{"name": l.name, "obs_dim": l.obs_dim, "num_actions": l.num_actions}
                                 for l in layouts]}}


def spec_of(stage: dict) -> SimpleNamespace:
    return SimpleNamespace(layouts=tuple(Layout(name, obs, actions)
                                         for name, (obs, actions) in zip(ve.NAMES, shapes(stage))), state_dim=4)


def test_seeding_from_a_stage_without_the_list_carries_the_camera_and_starts_the_list_fresh():
    """M3 seeds from M2 (no sight block): the camera's encoder, its entity list and join carry, the layouts' own
    blocks carry, and the sight list starts fresh with its pool at zero -- whatever the list holds, the seeded policy's
    own actions are what they were."""
    config = MappoConfig(hidden=(16, 16))
    old_stage, new_stage = sight_stage(False), sight_stage()
    torch.manual_seed(0)
    m2 = MappoTrainer(shapes(old_stage), 4, config, vision=vision_of(old_stage, ve.NAMES))
    with torch.no_grad():
        m2.actor.vision.embed.bias.normal_()
        m2.actor.vision.entities.encoders["visible"][0].weight.normal_()
        m2.actor.vision_join.linear.weight.normal_()
        m2.actor.heads[0].bias.normal_()
    m3 = MappoTrainer(shapes(new_stage), 4, config, vision=vision_of(new_stage, ve.NAMES))
    seed_trainer(m3, checkpoint_of(m2, old_stage), spec_of(new_stage), new_stage)
    torch.testing.assert_close(m3.actor.vision.embed.bias, m2.actor.vision.embed.bias)
    torch.testing.assert_close(m3.actor.vision.entities.encoders["visible"][0].weight,
                               m2.actor.vision.entities.encoders["visible"][0].weight)
    torch.testing.assert_close(m3.actor.vision_join.linear.weight, m2.actor.vision_join.linear.weight)
    torch.testing.assert_close(m3.actor.heads[0].bias[:BASE_ACTIONS], m2.actor.heads[0].bias)
    pool = m3.actor.vision.sight.pool
    assert bool((pool.weight == 0).all()) and bool((pool.bias == 0).all())

    obs, layout, image = rows(6)
    mask = torch.ones(6, ACTIONS)
    other = obs.clone()
    for row in range(6):
        name = ve.NAMES[int(layout[row])]
        if name in SIGHT_FIRST:
            other[row, SIGHT_FIRST[name]: SIGHT_FIRST[name] + SLOTS * WIDTH] = torch.randn(SLOTS * WIDTH)
    with torch.no_grad():
        # Relative to one another (the presses' logits share the normalisation).
        before = m3.actor(obs, layout, mask, image=image).logits[:, :BASE_ACTIONS]
        after = m3.actor(other, layout, mask, image=image).logits[:, :BASE_ACTIONS]
        torch.testing.assert_close(before - before[:, :1], after - after[:, :1])

    # And a stage with the list seeds from one with it: the list and its pointers carry.
    with torch.no_grad():
        m3.actor.vision.sight.pool.weight.normal_()
        m3.actor.sight_pointers.queries["select"].weight.normal_()
    again = MappoTrainer(shapes(new_stage), 4, config, vision=vision_of(new_stage, ve.NAMES))
    seed_trainer(again, checkpoint_of(m3, new_stage), spec_of(new_stage), new_stage)
    torch.testing.assert_close(again.actor.vision.sight.pool.weight, m3.actor.vision.sight.pool.weight)
    torch.testing.assert_close(again.actor.sight_pointers.queries["select"].weight,
                               m3.actor.sight_pointers.queries["select"].weight)
