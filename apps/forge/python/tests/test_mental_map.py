"""The mental map's crop (perception-goals REDESIGN §3, the Change; protocol 24): its bytes on the wire after the
camera's images and joined to them as the learner keeps a camera row, its decode against the sim's encoding
(Vision::MentalMap::Crop, DecodeCropCell), stage.json's map block, the map encoder's shapes inside the shared camera
encoder, seeding (the map fresh with its join at zero, the camera and the move block carried), the VIN switch, and
the buffer keeping the rows as bytes."""

import dataclasses
import struct
from types import SimpleNamespace

import numpy as np
import pytest
import torch

import test_vision_encoder as ve
from animus import protocol as p
from animus.bootstrap import seed_trainer
from animus.mappo.buffer import RolloutBuffer
from animus.mappo.networks import (MapEncoder, MapValueIteration, check_image_bytes, decode_map, vision_image_bytes,
                                   vision_of, with_map_vin)
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout

# A small crop for speed (the sim's is 48 x 48): the encoder takes any even size.
MH, MW = 16, 16
M = MH * MW * 6
MAP_SCALARS = 4
CROP = {"transport": "bytes", "height": MH, "width": MW, "cell": 2.0, "heading_up": True, "channels": 6,
        "map_bytes": M, "codes": 5, "classes": 32, "scalars": MAP_SCALARS, "code_channel": 0, "height_channel": 1,
        "height_step": 0.25, "height_zero": 128, "visited_channel": 2, "age_channel": 3, "age_scale": 20.0,
        "age_never": 255, "class_channel": 4, "frontier_channel": 5}

# Vision/MentalMap.h, ported as written.
CROP_HEIGHT_STEP, CROP_HEIGHT_ZERO, CROP_AGE_SCALE, CROP_AGE_NEVER = 0.25, 128, 20.0, 255


def map_stage(crop: dict | None = CROP) -> dict:
    """test_vision_encoder's stage with a map block after the entity list (its four scalars), before the goal."""
    stage = ve.stage()
    for name in ("warrior", "priest"):
        entry = stage["layouts"][name]
        goal = entry["blocks"].pop()
        at = goal["obs"][0]
        if crop is not None:
            entry["blocks"].append(ve.block("map", at, MAP_SCALARS, revision=1, map=dict(crop)))
            at += MAP_SCALARS
        entry["blocks"].append(ve.block("goal", at, 2))
        entry["obs_dim"] = at + 2
    return stage


MAP_FIRST = {"warrior": ve.FIRST["warrior"] + ve.SPAN + ve.SLOTS * ve.FEATURES,
             "priest": ve.FIRST["priest"] + ve.SPAN + ve.SLOTS * ve.FEATURES}


def crops(rows: int, seed: int = 0) -> torch.Tensor:
    """Plausible crops [rows, M]: codes 0-4, heights round the feet or none, visited, ages, classes, frontier."""
    g = torch.Generator().manual_seed(seed)
    cells = torch.zeros(rows, MH, MW, 6, dtype=torch.uint8)
    cells[..., 0] = torch.randint(0, 5, (rows, MH, MW), generator=g, dtype=torch.uint8)
    cells[..., 1] = torch.randint(0, 256, (rows, MH, MW), generator=g, dtype=torch.uint8)
    cells[..., 2] = torch.randint(0, 2, (rows, MH, MW), generator=g, dtype=torch.uint8)
    cells[..., 3] = torch.randint(0, 256, (rows, MH, MW), generator=g, dtype=torch.uint8)
    cells[..., 4] = torch.randint(0, 23, (rows, MH, MW), generator=g, dtype=torch.uint8)
    cells[..., 5] = torch.randint(0, 2, (rows, MH, MW), generator=g, dtype=torch.uint8)
    return cells.reshape(rows, -1)


def map_rows(rows: int, seed: int = 0, layouts=None):
    """test_vision_encoder.observations with each row's map: (obs, layout, camera rows of image + crop)."""
    s = map_stage()
    obs, layout, image = ve.observations(rows, seed, layouts)
    width = max(s["layouts"][name]["obs_dim"] for name in ve.NAMES)
    wide = torch.zeros(rows, width)
    wide[:, : obs.shape[1]] = obs
    return wide, layout, torch.cat([image, crops(rows, seed)], dim=1)


def shapes() -> list[tuple[int, int]]:
    return ve.shapes(map_stage())


# ------------------------------------------------------------------ the bytes


def encode_cell(code: int, height: float | None, visited: bool, age: float | None, entity: int,
                frontier: bool) -> list[int]:
    """One crop cell's six bytes as Vision::MentalMap::Crop writes them."""
    rise = 0 if height is None else int(np.clip(CROP_HEIGHT_ZERO + round(height / CROP_HEIGHT_STEP), 1, 255))
    aged = CROP_AGE_NEVER if age is None else int(min(254.0, round(CROP_AGE_SCALE * np.log2(1.0 + age))))
    return [code, rise, int(visited), aged, entity, int(frontier)]


def test_the_crop_decodes_as_the_sim_encodes_it():
    cells = [encode_cell(2, -2.0, True, 0.0, 16, False), encode_cell(1, 1.5, False, 600.0, 0, True),
             encode_cell(0, None, False, None, 0, False), encode_cell(3, 31.75, False, 3600.0, 3, False)]
    raw = torch.tensor(cells + [[0, 0, 0, 255, 0, 0]] * (MH * MW - len(cells)), dtype=torch.uint8).reshape(1, -1)
    decoded = decode_map(raw, MH, MW)
    assert decoded["code"].shape == (1, MH, MW) and decoded["values"].shape == (1, MH, MW, 5)
    flat = lambda name: decoded[name].reshape(-1, *decoded[name].shape[3:])
    assert flat("code")[:4].tolist() == [2, 1, 0, 3]
    assert flat("class")[:4].tolist() == [16, 0, 0, 3]
    values = flat("values")
    # Height over the feet in steps of 127 (the byte's range), 0 and "no floor" without one.
    torch.testing.assert_close(values[:4, 0], torch.tensor([-8 / 127, 6 / 127, 0.0, 127 / 127]))
    assert values[:4, 1].tolist() == [1.0, 1.0, 0.0, 1.0]
    assert values[:4, 2].tolist() == [1.0, 0.0, 0.0, 0.0]
    # The age: the log byte over 255, never seen 1.
    torch.testing.assert_close(values[:4, 3], torch.tensor([0.0, 185 / 255, 1.0, 236 / 255]))
    assert values[:4, 4].tolist() == [0.0, 1.0, 0.0, 0.0]


SPEC = dataclasses.replace(ve_spec := p.Spec(
    version=p.PROTOCOL_VERSION, num_envs=3, agents_per_env=2, obs_dim=4, state_dim=5, num_actions=3,
    episode_info_dim=2, goal_count=0, tick_ms=50, decision_ticks=1, episode_seconds=60, scenario="mapping",
    layouts=(p.Layout("warrior", 4, 3),), episode_info_names=("found",), kinematics_dim=10, image_bytes=20,
    look_heads=3), map_bytes=12)    # four pixels of five bytes; two cells of six


def test_spec_carries_the_map_bytes_last():
    payload = p.encode_spec(SPEC)
    assert payload[84:96] == struct.pack("<III", 20, 3, 12)
    assert p.decode_spec(payload) == SPEC
    assert SPEC.camera_bytes == 32 and ve_spec.camera_bytes == 20


def test_a_step_with_a_map_round_trips_its_own_section_after_the_images():
    rng = np.random.default_rng(3)
    e, a = SPEC.num_envs, SPEC.agents_per_env
    done = np.array([False, True, False])
    image = rng.integers(0, 256, (e, a, 20), dtype=np.uint8)
    final_image = rng.integers(0, 256, (e, a, 20), dtype=np.uint8)
    crop = rng.integers(0, 256, (e, a, 12), dtype=np.uint8)
    final_crop = rng.integers(0, 256, (e, a, 12), dtype=np.uint8)
    step = p.Step(decision=5, obs=np.zeros((e, a, 4), np.float32), state=np.zeros((e, 5), np.float32),
                  mask=np.ones((e, a, 3), bool), layout=np.zeros((e, a), np.uint16), present=np.ones((e, a), bool),
                  reward=np.zeros((e, a), np.float32), done=done, terminated=done,
                  final_obs=np.zeros((e, a, 4), np.float32), final_state=np.zeros((e, 5), np.float32),
                  episode_info=np.zeros((e, a, 2), np.float32),
                  episode_seed=np.full(e, p.NO_EPISODE_SEED, np.uint32),
                  kinematics=np.zeros((e, a, 10), np.float32), image=image, final_image=final_image, map=crop,
                  final_map=final_crop)
    payload = p.encode_step(SPEC, step)
    # The wire's order: ..., kinematics, image, final_image (ended), map, final_map (ended).
    tail = image.tobytes() + final_image[done].tobytes() + crop.tobytes() + final_crop[done].tobytes()
    assert payload.endswith(tail)
    assert len(payload) == SPEC.step_payload_size(ended=1)
    decoded = p.decode_step(SPEC, payload)
    # The learner's camera row: the image, then the crop.
    np.testing.assert_array_equal(decoded.image, np.concatenate([image, crop], axis=-1))
    np.testing.assert_array_equal(decoded.final_image[done], np.concatenate([final_image, final_crop], -1)[done])
    np.testing.assert_array_equal(decoded.map, crop)
    # Joined rows encode back to the same wire.
    assert p.encode_step(SPEC, dataclasses.replace(decoded, map=None, final_map=None)) == payload
    # With device buffers the images leave the STEP; the map stays on the socket.
    names = [name for name, _, _ in SPEC.step_layout(device=True)]
    assert "image" not in names and names[-3:] == ["final_image", "map", "final_map"]


def test_a_stage_without_a_map_is_protocol_23_on_the_wire():
    names = [name for name, _, _ in ve_spec.step_layout()]
    assert names[-2:] == ["image", "final_image"]
    assert p.no_camera((2, 1, 20), ve_spec).shape == (2, 1, 20)
    blank = p.no_camera((2, 1, 32), SPEC)
    assert blank[..., :5].tolist() == [[list(p.NO_FRAME_PIXEL)]] * 2 and not blank[..., 20:].any()


# ------------------------------------------------------------------ stage.json


def test_vision_of_reads_the_map_block():
    vision = vision_of(map_stage(), ve.NAMES)
    crop = vision[0]["map"]
    assert crop["first"] == MAP_FIRST["warrior"] and vision[1]["map"]["first"] == MAP_FIRST["priest"]
    assert (crop["height"], crop["width"], crop["channels"], crop["map_bytes"]) == (MH, MW, 6, M)
    assert vision[0]["camera_bytes"] == ve.BYTES + M and vision_image_bytes(vision) == ve.BYTES + M
    check_image_bytes(vision, ve.BYTES, M)
    with pytest.raises(ValueError, match="map bytes"):
        check_image_bytes(vision, ve.BYTES, 0)
    with pytest.raises(ValueError, match="map bytes"):
        check_image_bytes(vision_of(ve.stage(), ve.NAMES), ve.BYTES, M)
    with pytest.raises(ValueError, match="channels"):
        vision_of(map_stage({**CROP, "channels": 5}), ve.NAMES)
    assert with_map_vin(vision, False) is vision
    assert with_map_vin(vision, True)[0]["map"]["vin"] and with_map_vin(vision, True)[2] is None


# ------------------------------------------------------------------ the encoder


def test_the_map_encoder_shapes_and_gradients():
    torch.manual_seed(0)
    vision = vision_of(map_stage(), ve.NAMES)
    actor = ve.LayoutActor(shapes(), [16, 16], vision=vision)
    encoder = actor.vision.map
    assert isinstance(encoder, MapEncoder) and encoder.vin is None
    assert encoder.class_embedding is actor.vision.class_embed     # one class table for pixels, list and map
    obs, layout, rows = map_rows(6)
    planes = encoder.planes(rows[:, ve.BYTES:])
    assert planes.shape == (6, MH, MW, MapEncoder.CODE_EMBED + 6 + 5)
    assert encoder.patches(planes).shape == (6, (MH // 4) * (MW // 4), 16 * (MapEncoder.CODE_EMBED + 6 + 5))
    out = encoder(obs, layout, rows[:, ve.BYTES:])
    assert out.shape == (6, actor.vision.EMBED)
    # Its scalar columns are blind to the adapters, as the camera's are.
    assert set(range(MAP_FIRST["warrior"], MAP_FIRST["warrior"] + MAP_SCALARS)) <= set(actor.vision.blind[0])
    mask = torch.ones(6, 5)
    actor(obs, layout, mask, image=rows).logits.sum().backward()
    for parameter in (encoder.patch.weight, encoder.embed.weight, encoder.join.weight, encoder.code_embed.weight):
        assert bool((parameter.grad != 0).any())
    # What the map holds moves the policy.
    with torch.no_grad():
        other = torch.cat([rows[:, : ve.BYTES], crops(6, seed=9)], dim=1)
        assert not torch.allclose(actor(obs, layout, mask, image=rows).logits,
                                  actor(obs, layout, mask, image=other).logits)


def test_the_full_size_crop():
    """The sim's own crop, 48 x 48 at 2 yd: the encoder's grid and its output."""
    crop = {**CROP, "height": 48, "width": 48, "map_bytes": 48 * 48 * 6}
    encoder = MapEncoder(crop, torch.nn.Embedding(32, 6), 256, [0])
    assert encoder.grid == (12, 12)
    raw = torch.randint(0, 256, (3, 48 * 48 * 6), dtype=torch.uint8)
    assert encoder(torch.rand(3, 8), torch.zeros(3, dtype=torch.long), raw).shape == (3, 256)


def test_the_buffer_keeps_camera_rows_with_their_map_as_bytes():
    buffer = RolloutBuffer(4, 3, 2, 5, 6, 3, image_bytes=ve.BYTES + M)
    assert buffer.image.dtype == np.uint8 and buffer.image.shape[-1] == ve.BYTES + M


def test_rollout_graphs_stay_on_with_a_map():
    trainer = MappoTrainer(shapes(), 4, MappoConfig(hidden=(16, 16)), vision=vision_of(map_stage(), ve.NAMES))
    assert trainer.image_bytes == ve.BYTES + M
    trainer._rollout_stream = object()
    assert trainer._graphs_apply(trainer.acting_state(2, 1))


# ------------------------------------------------------------------ seeding and the VIN


def checkpoint_of(trainer: MappoTrainer, stage: dict) -> dict:
    layouts = [Layout(name, obs, actions) for name, (obs, actions) in zip(ve.NAMES, ve.shapes(stage))]
    return {"trainer": trainer.state_dict(), "stage": stage,
            "spec": {"layouts": [{"name": l.name, "obs_dim": l.obs_dim, "num_actions": l.num_actions}
                                 for l in layouts]}}


def spec_of(stage: dict) -> SimpleNamespace:
    return SimpleNamespace(layouts=tuple(Layout(name, obs, actions)
                                         for name, (obs, actions) in zip(ve.NAMES, ve.shapes(stage))), state_dim=4)


def test_seeding_m2_from_m1_starts_the_map_fresh_and_carries_the_camera():
    """M2 seeds from M1 (REDESIGN §4): the map encoder starts fresh with its join at zero, so the seeded policy acts as
    M1's did whatever the map holds; the camera's encoder and join carry."""
    config = MappoConfig(hidden=(16, 16))
    torch.manual_seed(0)
    m1 = MappoTrainer(ve.shapes(ve.stage()), 4, config, vision=vision_of(ve.stage(), ve.NAMES))
    with torch.no_grad():
        m1.actor.vision.embed.bias.normal_()
        m1.actor.vision_join.linear.weight.normal_()
    m2 = MappoTrainer(shapes(), 4, config, vision=vision_of(map_stage(), ve.NAMES))
    seed_trainer(m2, checkpoint_of(m1, ve.stage()), spec_of(map_stage()), map_stage())
    torch.testing.assert_close(m2.actor.vision.embed.bias, m1.actor.vision.embed.bias)
    torch.testing.assert_close(m2.actor.vision_join.linear.weight, m1.actor.vision_join.linear.weight)
    join = m2.actor.vision.map.join
    assert bool((join.weight == 0).all()) and bool((join.bias == 0).all())
    obs, layout, rows = map_rows(6)
    mask = torch.ones(6, 5)
    with torch.no_grad():
        other = torch.cat([rows[:, : ve.BYTES], crops(6, seed=9)], dim=1)
        torch.testing.assert_close(m2.actor(obs, layout, mask, image=rows).logits,
                                   m2.actor(obs, layout, mask, image=other).logits)


def test_the_vin_switch():
    """mappo.map_vin: off by default; on, the map encoder has a value iteration network whose read-out starts at zero,
    and a checkpoint without one seeds the map and leaves the VIN fresh, the policy as it was."""
    assert not MappoConfig().map_vin
    config = MappoConfig(hidden=(16, 16))
    torch.manual_seed(0)
    plain = MappoTrainer(shapes(), 4, config, vision=vision_of(map_stage(), ve.NAMES))
    with torch.no_grad():
        plain.actor.vision.map.join.weight.normal_()
    vin_vision = with_map_vin(vision_of(map_stage(), ve.NAMES), True)
    with_vin = MappoTrainer(shapes(), 4, dataclasses.replace(config, map_vin=True), vision=vin_vision)
    vin = with_vin.actor.vision.map.vin
    assert isinstance(vin, MapValueIteration)
    assert bool((vin.out.weight == 0).all())
    seed_trainer(with_vin, checkpoint_of(plain, map_stage()), spec_of(map_stage()), map_stage())
    vin = with_vin.actor.vision.map.vin             # seeding loads into networks of its own
    torch.testing.assert_close(with_vin.actor.vision.map.join.weight, plain.actor.vision.map.join.weight)
    assert bool((with_vin.actor.vision.map.vin.out.weight == 0).all())
    obs, layout, rows = map_rows(6)
    mask = torch.ones(6, 5)
    with torch.no_grad():
        torch.testing.assert_close(with_vin.actor(obs, layout, mask, image=rows).logits,
                                   plain.actor(obs, layout, mask, image=rows).logits)
    # The value map reaches across the crop and differs by the goal (the object seen or not).
    middle = torch.randn(2, MapEncoder.WIDTHS[1], MH // 4, MW // 4)
    with torch.no_grad():
        assert vin.values(middle, torch.tensor([0, 0])).shape == (2, 1, MH // 4, MW // 4)
        assert not torch.allclose(vin.values(middle, torch.tensor([0, 0])), vin.values(middle, torch.tensor([1, 1])))
    # Its gradient reaches it once its read-out is trained.
    with torch.no_grad():
        vin.out.weight.normal_()
    with_vin.actor(obs, layout, mask, image=rows).logits.sum().backward()
    assert bool((vin.transition.weight.grad != 0).any())
