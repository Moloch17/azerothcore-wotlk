"""Identity on the camera (perception-goals P2, vision block revision 5, protocol 23): the slot byte decoded apart
from the image channels, the class embedded, the entity list read as a set with its class and type embedded, each
listed entity linked to the patch features under its own pixels, STEPs with five-byte images round-tripping, and a
stage without a camera exactly as before."""

import dataclasses

import numpy as np
import pytest
import torch

from animus import protocol as p
from animus.mappo.networks import VisionEncoder, decode_slots, vision_of
from test_vision_encoder import FEATURES, FIRST, H, NAMES, SCALARS, SLOTS, W, observations, stage


def encoder() -> VisionEncoder:
    torch.manual_seed(0)
    return VisionEncoder(vision_of(stage(), NAMES))


def test_the_slot_byte_is_its_own_and_no_image_channel():
    enc = encoder()
    assert enc.planes_per_pixel == 4 + VisionEncoder.CLASS_EMBED      # distance, height, normal, objective, class
    image = torch.zeros(1, H * W * 5, dtype=torch.uint8)
    image[0, 4::5] = 2
    assert bool((decode_slots(image, H, W) == 2).all())
    # Two frames that differ only in their slots decode to the same image channels.
    other = image.clone()
    other[0, 4::5] = 0
    obs, layout, _ = observations(1, layouts=[0])
    assert torch.equal(enc.gather(obs, layout, image)[0], enc.gather(obs, layout, other)[0])


def test_each_slot_pools_the_patch_features_under_its_own_pixels():
    """1c: slot s's link is the mean of the patch features weighted by its pixels in each patch."""
    enc = encoder()
    patches = (H // 4) * (W // 4)
    features = torch.arange(patches * 2, dtype=torch.float32).reshape(1, patches, 2)
    slots = torch.zeros(1, H, W, dtype=torch.long)
    slots[0, 0:4, 0:4] = 1                  # all of patch 0
    slots[0, 0:2, 4:8] = 2                  # half of patch 1 ...
    slots[0, 4:8, 8:9] = 2                  # ... and a quarter of patch 5
    linked = enc.link_features(features, slots)
    assert linked.shape == (1, SLOTS, 2)
    torch.testing.assert_close(linked[0, 0], features[0, 0])
    expected = (8 * features[0, 1] + 4 * features[0, 5]) / 12
    torch.testing.assert_close(linked[0, 1], expected)
    torch.testing.assert_close(linked[0, 2], torch.zeros(2))        # no pixel, nothing


def entity_rows(rows: int, present: int, layout: int = 0) -> torch.Tensor:
    """Observation rows of a camera layout with `present` listed entities (classes 6 + k, types 100 + k)."""
    obs, _, _ = observations(rows, layouts=[layout] * rows)
    first = FIRST[NAMES[layout]] + SCALARS
    obs[:, first: first + SLOTS * FEATURES] = 0.0
    for slot in range(present):
        at = first + slot * FEATURES
        obs[:, at] = 1.0
        obs[:, at + 1] = 6.0 + slot
        obs[:, at + 2] = 100.0 + slot
    return obs


def test_the_list_is_a_set_of_its_present_slots_with_embedded_classes_and_types():
    enc = encoder()
    entities = enc.entities
    assert entities is not None and entities.slots == SLOTS
    # One shared class table for the pixels and the list; the type hashed into its own buckets.
    assert entities.class_embedding is enc.class_embed
    assert entities.type_embed.num_embeddings == 64
    obs = entity_rows(2, 2)
    layout = torch.zeros(2, dtype=torch.long)
    linked = torch.zeros(2, SLOTS, VisionEncoder.WIDTHS[1])
    codes, present = entities.encode_linked(obs, layout, linked)["visible"]
    assert codes.shape == (2, SLOTS, 64)
    assert present.tolist() == [[True, True, False]] * 2
    # A different class or type is a different token; the link moves it too.
    other = obs.clone()
    other[:, FIRST["warrior"] + SCALARS + 1] = 9.0
    assert not torch.allclose(entities.encode_linked(other, layout, linked)["visible"][0][:, 0], codes[:, 0])
    retyped = obs.clone()
    retyped[:, FIRST["warrior"] + SCALARS + 2] = 7.0
    assert not torch.allclose(entities.encode_linked(retyped, layout, linked)["visible"][0][:, 0], codes[:, 0])
    moved = entities.encode_linked(obs, layout, torch.ones_like(linked))["visible"][0]
    assert not torch.allclose(moved[:, 0], codes[:, 0])


def test_the_list_reaches_the_embedding_and_its_gradient_the_list_encoder():
    enc = encoder()
    obs = entity_rows(4, 2)
    layout = torch.zeros(4, dtype=torch.long)
    _, _, image = observations(4, layouts=[0] * 4)
    with torch.no_grad():
        alone = enc(entity_rows(4, 0), layout, image)
        listed = enc(obs, layout, image)
    assert not torch.allclose(alone, listed)
    enc(obs, layout, image).sum().backward()
    assert bool((enc.entities.link.weight.grad != 0).any())
    assert bool((enc.entities.type_embed.weight.grad != 0).any())
    assert bool((enc.patch.weight.grad != 0).any())
    # The list's columns are blind to the adapters: the encoder reads them, nothing else.
    first = FIRST["warrior"] + SCALARS
    assert set(range(first, first + SLOTS * FEATURES)) <= set(enc.blind[0])


def test_steps_with_five_byte_images_round_trip():
    spec = p.Spec(version=p.PROTOCOL_VERSION, num_envs=2, agents_per_env=1, obs_dim=3, state_dim=2, num_actions=2,
                  episode_info_dim=1, goal_count=0, tick_ms=50, decision_ticks=1, episode_seconds=60,
                  scenario="identity", layouts=(p.Layout("warrior", 3, 2),), episode_info_names=("x",),
                  kinematics_dim=10, image_bytes=H * W * 5, look_heads=3)
    assert p.PROTOCOL_VERSION == 25 and len(p.NO_FRAME_PIXEL) == 5
    assert p.decode_spec(p.encode_spec(spec)) == spec
    rng = np.random.default_rng(3)
    image = rng.integers(0, 256, (2, 1, spec.image_bytes), dtype=np.uint8)
    done = np.array([False, True])
    step = p.Step(decision=1, env_begin=0, obs=np.zeros((2, 1, 3), np.float32), state=np.zeros((2, 2), np.float32),
                  mask=np.ones((2, 1, 2), bool), layout=np.zeros((2, 1), np.uint16), present=np.ones((2, 1), bool),
                  reward=np.zeros((2, 1), np.float32), done=done, terminated=done,
                  final_obs=np.zeros((2, 1, 3), np.float32), final_state=np.zeros((2, 2), np.float32),
                  episode_info=np.zeros((2, 1, 1), np.float32),
                  episode_seed=np.full(2, p.NO_EPISODE_SEED, np.uint32), image=image,
                  final_image=p.no_frame((2, 1, spec.image_bytes)),
                  kinematics=np.zeros((2, 1, 10), np.float32))
    decoded = p.decode_step(spec, p.encode_step(spec, step))
    np.testing.assert_array_equal(decoded.image, image)
    assert tuple(decoded.final_image[1, 0, :5]) == (255, 128, 0, 0, 0)


def test_a_stage_without_a_camera_has_no_image_on_the_wire():
    plain = p.Spec(version=p.PROTOCOL_VERSION, num_envs=1, agents_per_env=1, obs_dim=3, state_dim=2, num_actions=2,
                   episode_info_dim=1, goal_count=0, tick_ms=50, decision_ticks=1, episode_seconds=60,
                   scenario="plain", layouts=(p.Layout("warrior", 3, 2),), episode_info_names=("x",),
                   kinematics_dim=10)
    camera = dataclasses.replace(plain, image_bytes=H * W * 5, look_heads=3)
    assert camera.step_payload_size(ended=1) - plain.step_payload_size(ended=1) == 2 * H * W * 5
    assert vision_of(stage(None), NAMES) is None


def test_a_slot_past_the_list_reads_as_its_last():
    """The sim never writes a slot past the list; noise (the bench's random bytes) is clamped, never an error."""
    enc = encoder()
    patches = (H // 4) * (W // 4)
    slots = torch.full((1, H, W), 200, dtype=torch.long)
    linked = enc.link_features(torch.ones(1, patches, 2), slots)
    torch.testing.assert_close(linked[0, -1], torch.ones(2))
    with pytest.raises(Exception):
        enc.link_features(torch.ones(1, patches, 2), slots.reshape(1, -1)[:, :5])
