"""The camera's encoder (camera-vision): stage.json's vision block read per layout, the image's bytes decoded on the
device into [N, H, W, C], the class embedded, the entity list linked to its pixels, the scalar and list columns kept
from the adapters and their normalisers, the join,
the encoder shared by the actor and the critic, the rollout graph kept on, export refused, and the networks of a stage
without a camera exactly as they were."""

import copy
import json
from types import SimpleNamespace

import numpy as np
import pytest
import torch

from animus.bootstrap import seed_trainer
from animus.export import export_layouts
from animus.mappo.networks import (LayoutActor, LayoutCritic, VisionEncoder, VisionJoin, owns_vision, update_norms,
                                   vision_of, without_blind_columns)
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout

H, W, C, CLASSES, CLASS_CHANNEL, SCALARS = 8, 12, 5, 23, 3, 7
BYTES = H * W * 5
SPAN = SCALARS
IMAGE = {"height": H, "width": W, "channels": C, "classes": CLASSES, "class_channel": CLASS_CHANNEL, "class_limit": 32,
         "scalars": SCALARS, "transport": "bytes", "bytes_per_pixel": 5}
# The entity list after the camera (perception-goals 1b): a short one here, its columns as the sim's.
SLOTS, FEATURES = 3, 20
ENTITY_NAMES = ["present", "class", "type", "object", "level", "level_delta", "health", "reaction", "quest", "lootable",
                "usable", "distance", "yaw_sin", "yaw_cos", "pitch_sin", "pitch_cos", "centroid_x", "centroid_y",
                "share", "memory"]
# Two layouts with a camera at different columns (their core blocks differ in width, as the classes' do) and one
# without.
NAMES = ["warrior", "priest", "director"]
FIRST = {"warrior": 8, "priest": 12}


def block(name: str, first: int, count: int, actions=(0, 0), **extra) -> dict:
    return {"name": name, "obs": [first, count], "actions": list(actions), **extra}


def entities(first: int) -> dict:
    return {"name": "visible", "slots": SLOTS, "width": FEATURES, "first": first, "present": 0, "class_column": 1,
            "type_column": 2, "classes": 32, "type_buckets": 64, "features": list(ENTITY_NAMES)}


def stage(image: dict | None = IMAGE, vision_revision: int = 5) -> dict:
    """A stage.json: warrior core 5 + move 3, priest core 9 + move 3, then the vision block (its 7 scalars; the image
    travels as bytes), its entity list (SLOTS of FEATURES) and a goal block of 2; the director core 6 + goal 2."""
    layouts = {}
    for name, core in (("warrior", 5), ("priest", 9)):
        blocks = [block("core", 0, core, (0, 3)), block("move", core, 3, (3, 2))]
        at = core + 3
        if image is not None:
            blocks.append(block("vision", at, SPAN, revision=vision_revision, image=dict(image)))
            at += SPAN
            blocks.append(block("entities", at, SLOTS * FEATURES, revision=1, entities=entities(at)))
            at += SLOTS * FEATURES
        blocks.append(block("goal", at, 2))
        layouts[name] = {"obs_dim": at + 2, "blocks": blocks}
    layouts["director"] = {"obs_dim": 8, "blocks": [block("core", 0, 6, (0, 4)), block("goal", 6, 2)]}
    return {"stage": "test_vision", "layouts": layouts}


def shapes(stage_json: dict) -> list[tuple[int, int]]:
    return [(stage_json["layouts"][name]["obs_dim"], 5 if name != "director" else 4) for name in NAMES]


def actor(vision=True, seed=0, **kwargs) -> LayoutActor:
    torch.manual_seed(seed)
    s = stage() if vision else stage(None)
    return LayoutActor(shapes(s), [16, 16], vision=vision_of(s, NAMES) if vision else None, **kwargs)


def frame(generator: torch.Generator) -> torch.Tensor:
    """A plausible frame's bytes [I]: any distance, height and normal, a class with the objective bit now and then,
    and a slot of the list or none."""
    pixels = torch.randint(0, 256, (H, W, 5), generator=generator, dtype=torch.uint8)
    kind = torch.randint(0, CLASSES, (H, W), generator=generator, dtype=torch.uint8)
    flag = (torch.rand(H, W, generator=generator) < 0.1).to(torch.uint8) << 5
    pixels[..., 3] = kind | flag
    pixels[..., 4] = torch.randint(0, SLOTS + 1, (H, W), generator=generator, dtype=torch.uint8)
    return pixels.reshape(-1)


def observations(rows: int, seed: int = 0, layouts=None) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    """Random rows of every layout, (obs, layout, image): a camera layout's scalars in [0, 1] and a frame of bytes;
    a layout without one the no-frame pattern."""
    s = stage()
    widths = [s["layouts"][name]["obs_dim"] for name in NAMES]
    generator = torch.Generator().manual_seed(seed)
    layout = (torch.tensor(layouts) if layouts is not None
              else torch.arange(rows) % len(NAMES))
    obs = torch.zeros(rows, max(widths))
    image = blank(rows, (255, 128, 0, 0, 0))
    for row in range(rows):
        index = int(layout[row])
        obs[row, : widths[index]] = torch.randn(widths[index], generator=generator)
        if NAMES[index] in FIRST:
            first = FIRST[NAMES[index]]
            obs[row, first : first + SCALARS] = torch.rand(SCALARS, generator=generator)
            image[row] = frame(generator)
    return obs, layout, image


def blank(rows: int, pixel) -> torch.Tensor:
    """`rows` frames of one pixel everywhere, [rows, I] uint8."""
    return torch.tensor(pixel, dtype=torch.uint8).repeat(rows, H * W)


# ------------------------------------------------------------------ stage.json


def test_vision_of_keeps_each_layouts_own_first_column():
    vision = vision_of(stage(), NAMES)
    assert [entry and entry["first"] for entry in vision] == [8, 12, None]
    # A manifest with no patch, render sizes or look: patch 4, none, none; and its entity list after the scalars.
    listed = {"first": 8 + SPAN, "slots": SLOTS, "width": FEATURES, "present": 0, "class_column": 1, "type_column": 2,
              "classes": 32, "type_buckets": 64, "object_column": 3}
    assert vision[0] == {"first": 8, **{k: v for k, v in IMAGE.items() if k != "transport"}, "image_bytes": BYTES,
                         "patch": 4, "render_sizes": (), "look": (), "look_names": (), "entities": listed,
                         "map": None, "camera_bytes": BYTES}
    assert vision[1]["entities"]["first"] == 12 + SPAN


def test_vision_of_is_none_without_a_camera():
    assert vision_of(stage(None), NAMES) is None
    assert vision_of(None, NAMES) is None


def test_vision_of_refuses_what_it_cannot_read():
    no_image = stage()
    del no_image["layouts"]["warrior"]["blocks"][2]["image"]
    with pytest.raises(ValueError, match="without its image"):
        vision_of(no_image, NAMES)
    other = stage()
    other["layouts"]["priest"]["blocks"][2]["image"] = {**IMAGE, "height": H + 1}
    with pytest.raises(ValueError, match="one encoder needs one shape"):
        vision_of(other, NAMES)
    short = stage()
    short["layouts"]["warrior"]["blocks"][2]["obs"][1] = SPAN - 1
    with pytest.raises(ValueError, match="its scalars"):
        vision_of(short, NAMES)
    floats = stage()
    del floats["layouts"]["warrior"]["blocks"][2]["image"]["transport"]
    with pytest.raises(ValueError, match="not sent as bytes"):
        vision_of(floats, NAMES)
    three = stage()
    three["layouts"]["warrior"]["blocks"][2]["image"] = {**IMAGE, "bytes_per_pixel": 3}
    with pytest.raises(ValueError, match="decodes 5 bytes"):
        vision_of(three, NAMES)
    # Revision 4's image: kinds, four bytes a pixel, no class table.
    kinds = stage(vision_revision=4)
    kinds["layouts"]["warrior"]["blocks"][2]["image"] = {k: v for k, v in IMAGE.items()
                                                         if k not in ("classes", "class_channel", "class_limit")}
    kinds["layouts"]["warrior"]["blocks"][2]["image"].update(kinds=8, kind_channel=3, bytes_per_pixel=4)
    with pytest.raises(ValueError, match="before revision 5"):
        vision_of(kinds, NAMES)
    unlisted = stage()
    unlisted["layouts"]["warrior"]["blocks"][3]["entities"]["slots"] = SLOTS + 1
    with pytest.raises(ValueError, match="its description"):
        vision_of(unlisted, NAMES)
    past = stage()
    past["layouts"]["warrior"]["obs_dim"] = FIRST["warrior"] + SPAN - 1
    with pytest.raises(ValueError, match="past the layout"):
        vision_of(past, NAMES)


def test_a_stage_without_a_camera_builds_the_networks_it_always_did():
    plain = stage(None)
    layouts = shapes(plain)
    torch.manual_seed(0)
    before = LayoutActor(layouts, [16, 16])
    torch.manual_seed(0)
    now = LayoutActor(layouts, [16, 16], vision=vision_of(plain, NAMES))
    assert now.vision is None
    assert list(now.state_dict()) == list(before.state_dict())
    for key, value in before.state_dict().items():
        torch.testing.assert_close(now.state_dict()[key], value)
    critic = LayoutCritic(4, layouts, [16, 16], vision=None)
    assert not any(key.startswith("vision") for key in critic.state_dict())
    config = MappoConfig(hidden=(16, 16))
    trainer = MappoTrainer(layouts, 4, config, vision=vision_of(plain, NAMES))
    assert not any("vision" in key for key in {**trainer.actor.state_dict(), **trainer.critic.state_dict()})


# ------------------------------------------------------------------ the encoder


def test_the_image_bytes_are_decoded_row_col_channel_and_the_scalars_read_from_each_layouts_columns():
    """The five image channels, decoded from the bytes exactly (Vision::DecodePixel), in the patch encoder's
    [N, H, W, C] -- the class from the class byte's low five bits, the objective from its bit 5, the slot byte apart;
    the scalars from each layout's own columns."""
    encoder = VisionEncoder(vision_of(stage(), NAMES))
    obs = torch.zeros(2, 12 + SPAN + SLOTS * FEATURES + 2)
    layout = torch.tensor([0, 1])
    image = blank(2, (255, 128, 0, 0, 0))               # no frame: sky, height 0
    row, col = 5, 6
    at = (row * W + col) * 5
    image[0, at : at + 5] = torch.tensor([127, 3, 51, 0x20 | 22, 2], dtype=torch.uint8)
    image[1, at : at + 5] = torch.tensor([0, 253, 255, 2, 0], dtype=torch.uint8)
    for index, name in enumerate(("warrior", "priest")):
        obs[index, FIRST[name] + 5] = 0.5 + index        # the sixth scalar (underwater)
    decoded, scalars = encoder.gather(obs, layout, image)
    assert decoded.shape == (2, H, W, C) and scalars.shape == (2, SCALARS)
    torch.testing.assert_close(decoded[0, row, col], torch.tensor([127 / 254, -125 / 125, 51 / 255, 22.0, 1.0]))
    torch.testing.assert_close(decoded[1, row, col], torch.tensor([0.0, 125 / 125, 1.0, 2.0, 0.0]))
    sky = torch.tensor([1.0, 0.0, 0.0, 0.0, 0.0])
    torch.testing.assert_close(decoded[0, 0, 0], sky)
    others = torch.ones(H, W, dtype=torch.bool)
    others[row, col] = False
    assert bool((decoded[:, others] == sky).all())
    for index in range(2):
        assert float(scalars[index, 5]) == 0.5 + index
        assert float(scalars[index].abs().sum()) == 0.5 + index


def test_a_network_with_a_camera_refuses_to_act_without_its_image():
    net = actor()
    obs, layout, _ = observations(3)
    with pytest.raises(ValueError, match="image bytes"):
        net(obs, layout, torch.ones(3, 5))


def test_the_class_channel_is_embedded_not_one_hot():
    """Perception-goals amendment 10: the class becomes a learned CLASS_EMBED-wide vector from a table of 32 (the
    wire's 5 bits), so a pixel is 4 + 6 planes whatever the number of classes."""
    encoder = VisionEncoder(vision_of(stage(), NAMES))
    assert encoder.class_embed.num_embeddings == 32 and encoder.class_embed.embedding_dim == VisionEncoder.CLASS_EMBED
    assert 4 <= VisionEncoder.CLASS_EMBED <= 8
    image = torch.zeros(1, H, W, C)
    image[0, ..., 0] = 0.7                              # distance
    image[0, ..., 4] = 1.0                              # objective
    classes = torch.tensor([0.0, 1.0, 2.6, 3.4, -1.0, 40.0, 22.0, 31.0])   # rounded, clamped to 0..31
    image[0, 0, : len(classes), CLASS_CHANNEL] = classes
    planes = encoder.planes(image)
    assert planes.shape == (1, H, W, C - 1 + VisionEncoder.CLASS_EMBED) == (1, H, W, 10)
    # The other channels in order (distance, height, normal, objective), then the class's embedding.
    torch.testing.assert_close(planes[0, ..., 0], image[0, ..., 0])
    torch.testing.assert_close(planes[0, ..., 3], image[0, ..., 4])
    expected = [0, 1, 3, 3, 0, 31, 22, 31]
    table = encoder.class_embed.weight.detach()
    for col, value in enumerate(expected):
        torch.testing.assert_close(planes[0, 0, col, 4:], table[value])
    torch.testing.assert_close(planes[0, 1:, :, 4:], table[0].expand(H - 1, W, -1))     # the rest is sky


def test_a_patch_is_its_square_of_pixels_row_by_row():
    encoder = VisionEncoder(vision_of(stage(), NAMES))
    planes = torch.zeros(1, H, W, 10)
    # Pixel (row 5, col 6) is in patch (1, 1) of the 2 x 3 grid -- the fifth patch -- at (1, 2) inside it.
    planes[0, 5, 6, 9] = 3.0
    patches = encoder.patches(planes)
    assert patches.shape == (1, (H // 4) * (W // 4), 4 * 4 * 10) == (1, 6, 160)
    assert float(patches[0, 1 * 3 + 1, (1 * 4 + 2) * 10 + 9]) == 3.0
    assert float(patches.abs().sum()) == 3.0


def test_an_image_the_patches_do_not_tile_is_refused():
    image = {**IMAGE, "height": 30}
    with pytest.raises(ValueError, match="multiples of 4"):
        VisionEncoder([{"first": 0, **image, "image_bytes": 30 * W * 5}])


def test_shapes_at_the_default_camera():
    image = {"height": 32, "width": 64, "channels": 5, "classes": 23, "class_channel": 3, "scalars": 7,
             "bytes_per_pixel": 5, "image_bytes": 32 * 64 * 5}
    descriptors = [{"first": 3, **image}, None]
    encoder = VisionEncoder(descriptors)
    join = VisionJoin(encoder.has_vision, 24)
    # 4 x 4 patches: 32 x 64 -> an 8 x 16 grid of 160 features each, 64 channels after the two layers.
    assert encoder.feature_shape == (64, 8, 16)
    assert (encoder.patch.in_features, encoder.patch.out_features) == (160, 64)
    assert encoder.entities is None                                    # no entity list described
    assert (encoder.mix.in_features, encoder.mix.out_features) == (64, 64)
    assert encoder.embed.in_features == 128 + 7 and encoder.embed.out_features == 256
    assert join.linear.in_features == 256 and join.linear.out_features == 24
    obs = torch.rand(5, 3 + 7)
    layout = torch.tensor([0, 1, 0, 1, 0])
    image = torch.randint(0, 256, (5, 32 * 64 * 5), dtype=torch.uint8)
    with torch.no_grad():
        join.linear.bias.normal_()
        embedding = encoder(obs, layout, image)
        out = join(embedding, layout)
        image = encoder.gather(obs, layout, image)[0]
        features = encoder.features(encoder.patches(encoder.planes(image)))
        points = encoder.keypoints(features)
    assert embedding.shape == (5, 256) and out.shape == (5, 24)
    assert bool((out[1] == 0).all()) and bool((out[3] == 0).all())     # no camera, nothing added
    assert bool((out[0] != 0).any())
    assert features.shape == (5, 128, 64)
    assert points.shape == (5, 128) and float(points.abs().max()) <= 1.0


# ------------------------------------------------------------------ the networks


def test_the_camera_columns_are_blind_to_the_adapters():
    net = actor()
    vision = vision_of(stage(), NAMES)
    obs, layout, image = observations(9)
    with torch.no_grad():
        net.vision_join.linear.weight.normal_()               # the camera in the logits, so gradient reaches everything
    logits = net(obs, layout, torch.ones(9, 5), image=image).logits
    logits.sum().backward()
    for index, entry in enumerate(vision):
        weight = net.adapters[index].weight
        if entry is None:
            assert getattr(net, f"vision_keep_{index}", None) is None
            continue
        columns = slice(entry["first"], entry["first"] + SPAN)
        assert bool((weight[:, columns] == 0).all())
        assert bool((weight.grad[:, columns] == 0).all())
        others = torch.ones(weight.shape[1], dtype=torch.bool)
        others[columns] = False
        assert bool((weight.grad[:, others] != 0).any())
    assert net.vision.patch.weight.grad is not None and bool((net.vision.patch.weight.grad != 0).any())


def test_the_camera_columns_keep_an_identity_normaliser():
    net = actor()
    obs, layout, _ = observations(30, seed=4)
    update_norms(net.norms, obs * 5.0 + 3.0, layout, net.obs_dims)
    for name, first in FIRST.items():
        norm = net.norms[NAMES.index(name)]
        assert float(norm.count) == 10
        torch.testing.assert_close(norm.mean[first : first + SPAN], torch.zeros(SPAN))
        torch.testing.assert_close(norm.var[first : first + SPAN], torch.ones(SPAN))
        assert bool((norm.mean[:first] != 0).all())     # the other columns did move
    # The rollout copies fold the normalisers into the adapters: the camera's columns stay zero there too.
    trainer = MappoTrainer(shapes(stage()), 4, MappoConfig(hidden=(16, 16)), vision=vision_of(stage(), NAMES))
    update_norms(trainer.actor.norms, obs * 5.0 + 3.0, layout, trainer.actor.obs_dims)
    trainer.sync_rollout()
    for name, first in FIRST.items():
        weight = trainer._rollout_actor.adapters[NAMES.index(name)].weight
        assert bool((weight[:, first : first + SPAN] == 0).all())


def test_a_fresh_camera_takes_part_from_the_first_update():
    """M1 trains from scratch (the user, 2026-10-06): its camera is initialised as the adapters are, so what it sees
    moves the logits at once and the whole encoder learns from the first update."""
    net = actor()
    obs, layout, image = observations(12)
    mask = torch.ones(12, 5)
    with torch.no_grad():
        # A frame with something in it against an even one (a spatial softmax reads where things are, so two even
        # frames of different colours look alike to it).
        dark = net(obs, layout, mask, image=blank(12, (0, 128, 0, 1, 0))).logits
        lit = net(obs, layout, mask, image=image).logits
    assert not torch.allclose(dark, lit)
    net(obs, layout, mask, image=image).logits.sum().backward()
    assert bool((net.vision_join.linear.weight.grad != 0).any())
    assert bool((net.vision.patch.weight.grad != 0).any())
    assert bool((net.vision.embed.weight.grad != 0).any())


def test_seeding_from_a_checkpoint_without_a_camera_leaves_the_policy_as_it_was():
    config = MappoConfig(hidden=(16, 16))
    layouts = [Layout(name, obs, actions) for name, (obs, actions) in zip(NAMES, shapes(stage()))]
    spec = SimpleNamespace(layouts=tuple(layouts), state_dim=4)
    checkpoint_spec = {"layouts": [{"name": l.name, "obs_dim": l.obs_dim, "num_actions": l.num_actions}
                                   for l in layouts]}
    # The checkpoint is from before the camera: its stage has no vision block, so its layouts are narrower.
    plain_stage = stage(None)
    plain_spec = {"layouts": [{"name": name, "obs_dim": obs, "num_actions": actions}
                              for name, (obs, actions) in zip(NAMES, shapes(plain_stage))]}
    torch.manual_seed(0)
    plain = MappoTrainer(shapes(plain_stage), 4, config)
    checkpoint = {"trainer": plain.state_dict(), "spec": plain_spec, "stage": plain_stage}
    seeded = MappoTrainer(shapes(stage()), 4, config, vision=vision_of(stage(), NAMES))
    seed_trainer(seeded, checkpoint, spec, stage())
    join = seeded.actor.vision_join.linear
    assert bool((join.weight == 0).all()) and bool((join.bias == 0).all())
    assert bool((seeded.critic.vision_join.linear.weight == 0).all())
    obs, layout, image = observations(12)
    mask = torch.ones(12, 5)
    with torch.no_grad():
        torch.testing.assert_close(seeded.actor(obs, layout, mask, image=image).logits,
                                   seeded.actor(obs, layout, mask, image=blank(12, (0, 128, 0, 1, 0))).logits)


def test_the_actor_and_the_critic_share_one_camera_encoder():
    trainer = MappoTrainer(shapes(stage()), 4, MappoConfig(hidden=(16, 16)), vision=vision_of(stage(), NAMES))
    assert trainer.actor.vision is not None and trainer.critic.vision is trainer.actor.vision
    assert owns_vision(trainer.actor) and not owns_vision(trainer.critic)
    # The rollout copies share theirs too, and it is not the trained one.
    assert trainer._rollout_critic.vision is trainer._rollout_actor.vision
    assert trainer._rollout_actor.vision is not trainer.actor.vision
    # One copy: in the actor's parameters and state dict alone; each network keeps its own join.
    encoder = {id(p) for p in trainer.actor.vision.parameters()}
    assert not encoder & {id(p) for p in trainer.critic.parameters()}
    assert not any(key.startswith("vision.") for key in trainer.critic.state_dict())
    assert any(key.startswith("vision_join.") for key in trainer.critic.state_dict())
    assert trainer.actor.vision_join is not trainer.critic.vision_join
    # Stepped by one optimizer: the camera's own, which neither the actor's nor the critic's holds.
    held = lambda optimizer: {id(p) for group in optimizer.param_groups for p in group["params"]}
    assert held(trainer.vision_opt) == encoder
    assert not encoder & held(trainer.actor_opt) and not encoder & held(trainer.critic_opt)
    obs, layout, image = observations(6)
    state = torch.randn(6, 4)
    with torch.no_grad():
        value = trainer.critic(state, obs, layout, image=image)
        trainer.actor.vision.embed.bias.add_(1.0)           # the actor's encoder moves the critic's value
        assert not torch.allclose(trainer.critic(state, obs, layout, image=image), value)


@pytest.mark.parametrize("two_clock", [False, True], ids=["plain", "two_clock"])
def test_both_losses_train_the_shared_camera_once_per_minibatch(two_clock):
    """The camera's gradient is the actor's and the critic's together, and vision_opt steps it once a minibatch. With
    the two-clock seat's goal update too, which replays the fast features (and so the images) itself."""
    extra = dict(goal_count=3, goal_every_decisions=2, slow_goal_size=4) if two_clock else {}
    config = MappoConfig(hidden=(16, 16), recurrent_size=4, epochs=1, minibatches=1, **extra)
    torch.manual_seed(0)
    trainer = MappoTrainer(shapes(stage()), 4, config, vision=vision_of(stage(), NAMES))
    obs, layout, image = observations(6)
    state = torch.randn(6, 4)
    encoder = trainer.actor.vision
    # Each loss alone reaches the encoder.
    trainer.critic(state, obs, layout, image=image).sum().backward()
    critic_grad = encoder.patch.weight.grad.clone()
    assert bool((critic_grad != 0).any())
    encoder.zero_grad()
    trainer.actor(obs, layout, torch.ones(6, 5), image=image).logits.sum().backward()
    assert bool((encoder.patch.weight.grad != 0).any())
    encoder.zero_grad()
    # In the update: one Adam step for the camera per minibatch (its state counts them).
    from animus.mappo.buffer import RolloutBuffer
    envs, steps = 3, 4
    buffer = RolloutBuffer(steps, envs, 2, obs.shape[-1], 4, 5, 0, trainer.recurrent_size, two_clock,
                           trainer.slow_goal_size, image_bytes=BYTES)
    assert buffer.image.dtype == np.uint8 and buffer.image.shape == (steps, envs, 2, BYTES)
    acting = trainer.acting_state(envs, 2)
    for step in range(steps):
        o, l, im = observations(envs * 2, seed=step)
        o, l, im = o.numpy().reshape(envs, 2, -1), l.numpy().reshape(envs, 2), im.numpy().reshape(envs, 2, -1)
        st = np.random.default_rng(step).standard_normal((envs, 4)).astype(np.float32)
        mask = np.ones((envs, 2, 5), bool)
        memory = acting.memory.copy()
        chosen, log_probs, values, _, goals, _ = trainer.act_and_value(o, mask, l, st, state=acting, image=im)
        buffer.add_decision(o, st, mask, l, chosen, log_probs, values, None, None, memory, goals, image=im)
        np.testing.assert_array_equal(buffer.image[step], im)
        buffer.add_outcome(np.ones((envs, 2), np.float32), np.zeros(envs, bool), np.zeros(envs, bool),
                           np.zeros((envs, 2), np.float32))
    buffer.finish(np.zeros((envs, 2), np.float32), 0.99, 0.95,
                  slow_goal=(config.slow_goal_gamma, config.slow_goal_lambda) if two_clock else None)
    before = [p.detach().clone() for p in encoder.parameters()]
    calls = []
    hook = encoder.register_forward_hook(lambda *_: calls.append(1))
    stats = trainer.update(buffer)
    hook.remove()
    # One minibatch, encoded once for the actor and the critic (and once more by the goal update, without gradient).
    assert len(calls) == (2 if two_clock else 1)
    assert stats["vision_grad_norm"] > 0.0
    assert stats["vision_grad_actor"] > 0.0 and stats["vision_grad_critic"] > 0.0
    assert all(int(trainer.vision_opt.state[p]["step"]) == 1 for p in encoder.parameters())
    assert any(not torch.equal(a, p.detach()) for a, p in zip(before, encoder.parameters()))
    # The rollout copies read the update's weights.
    torch.testing.assert_close(trainer._rollout_actor.vision.patch.weight, encoder.patch.weight)


def test_rollout_graphs_stay_on_with_a_camera():
    """The graph gate itself, with the GPU's stream stubbed: a camera must not turn graphs off (as the seat sets and
    the director do)."""
    trainer = MappoTrainer(shapes(stage()), 4, MappoConfig(hidden=(16, 16)), vision=vision_of(stage(), NAMES))
    assert trainer.vision is not None
    trainer._rollout_stream = object()
    state = trainer.acting_state(2, 1)
    assert trainer._graphs_apply(state)
    assert not trainer._graphs_apply(None)
    trainer.config.rollout_graphs = False
    assert not trainer._graphs_apply(state)
    trainer.config.rollout_graphs = True
    trainer.seat_sets = [[], [], []]                     # the gate still sees what does turn graphs off
    assert not trainer._graphs_apply(state)


@pytest.mark.skipif(not torch.cuda.is_available(), reason="rollout graphs need a GPU")
def test_the_graph_decides_with_the_camera_as_the_eager_path_does():
    torch.manual_seed(0)
    config = MappoConfig(hidden=(16, 16), recurrent_size=8)
    trainer = MappoTrainer(shapes(stage()), 4, config, train_device="cuda", rollout_device="cuda",
                           vision=vision_of(stage(), NAMES))
    with torch.no_grad():
        for network in (trainer.actor, trainer.critic):
            network.vision_join.linear.weight.normal_(std=0.5)
    trainer.sync_rollout()
    obs, layout, image = observations(6, layouts=[0, 1, 2, 0, 1, 2])
    arrays = (obs.numpy().reshape(3, 2, -1), np.ones((3, 2, 5), bool), layout.numpy().reshape(3, 2),
              np.random.default_rng(0).standard_normal((3, 4)).astype(np.float32))
    image = image.numpy().reshape(3, 2, -1)
    eager_state = trainer.acting_state(3, 2)
    graph_state = copy.deepcopy(eager_state)
    trainer.config.rollout_graphs = False
    eager = trainer.act_and_value(*arrays, deterministic=True, state=eager_state, image=image)
    trainer.config.rollout_graphs = True
    graphed = trainer.act_and_value(*arrays, deterministic=True, state=graph_state, image=image)
    assert trainer._rollout_graphs, "the decision was not captured"
    np.testing.assert_allclose(eager[2], graphed[2], rtol=1e-4, atol=1e-5)          # values
    np.testing.assert_allclose(eager_state.memory, graph_state.memory, rtol=1e-4, atol=1e-5)
    # The graph read the camera: another image, other values.
    other = trainer.act_and_value(*arrays, deterministic=True, state=copy.deepcopy(eager_state),
                                  image=blank(6, (0, 128, 0, 1, 0)).numpy().reshape(3, 2, -1))
    assert not np.allclose(other[2], graphed[2])


# ------------------------------------------------------------------ checkpoints and export


def test_a_checkpoint_round_trips_with_the_camera():
    vision = vision_of(stage(), NAMES)
    config = MappoConfig(hidden=(16, 16), recurrent_size=4)
    torch.manual_seed(0)
    saved = MappoTrainer(shapes(stage()), 4, config, vision=vision)
    with torch.no_grad():
        saved.actor.vision_join.linear.weight.normal_()
        saved.critic.vision_join.linear.bias.normal_()
        saved.actor.vision.embed.bias.normal_()
    state = saved.state_dict()
    assert any(key.startswith("vision.") for key in state["actor"])
    assert not any(key.startswith("vision.") for key in state["critic"])        # one copy, the actor's
    assert "vision_opt" in state
    assert any(key.startswith("vision_keep_") for key in state["actor"])
    assert not any(key.startswith("vision_keep_") for key in without_blind_columns(state["actor"]))
    torch.manual_seed(1)
    loaded = MappoTrainer(shapes(stage()), 4, config, vision=vision)
    loaded.load_state_dict(state)
    for network in ("actor", "critic"):
        for key, value in getattr(saved, network).state_dict().items():
            torch.testing.assert_close(getattr(loaded, network).state_dict()[key], value)
    assert loaded.critic.vision is loaded.actor.vision
    torch.testing.assert_close(loaded._rollout_critic.vision.embed.bias, saved.actor.vision.embed.bias)
    assert loaded.director_columns_clear()
    # A camera checkpoint does not load into networks without one, nor the other way round.
    plain = MappoTrainer(shapes(stage()), 4, config)
    with pytest.raises(RuntimeError):
        plain.load_state_dict(state)
    with pytest.raises(RuntimeError):
        loaded.load_state_dict(plain.state_dict())


def test_seeding_starts_the_camera_fresh_or_carries_it():
    config = MappoConfig(hidden=(16, 16))
    layouts = [Layout(name, obs, actions) for name, (obs, actions) in zip(NAMES, shapes(stage()))]
    spec = SimpleNamespace(layouts=tuple(layouts), state_dim=4)
    checkpoint_spec = {"layouts": [{"name": l.name, "obs_dim": l.obs_dim, "num_actions": l.num_actions}
                                   for l in layouts]}
    torch.manual_seed(0)
    old = MappoTrainer(shapes(stage()), 4, config, vision=vision_of(stage(), NAMES))
    with torch.no_grad():
        old.actor.vision_join.linear.weight.fill_(0.5)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec, "stage": stage()}

    carried = MappoTrainer(shapes(stage()), 4, config, vision=vision_of(stage(), NAMES))
    seed_trainer(carried, checkpoint, spec, stage())
    torch.testing.assert_close(carried.actor.vision_join.linear.weight, old.actor.vision_join.linear.weight)
    assert carried.director_columns_clear()

    revised = MappoTrainer(shapes(stage()), 4, config, vision=vision_of(stage(), NAMES))
    seed_trainer(revised, checkpoint, spec, stage(vision_revision=4))
    assert bool((revised.actor.vision_join.linear.weight == 0).all())
    assert revised.director_columns_clear()


def test_export_refuses_a_layout_with_a_camera(tmp_path):
    s = stage()
    stage_dir = tmp_path / "layouts" / "test_vision"
    stage_dir.mkdir(parents=True)
    (stage_dir / "stage.json").write_text(json.dumps(s))
    spec = {"scenario": "test_vision",
            "layouts": [{"name": name, "obs_dim": obs, "num_actions": actions}
                        for name, (obs, actions) in zip(NAMES, shapes(s))]}
    out = tmp_path / "models"
    out.mkdir()
    with pytest.raises(ValueError, match="the camera and its look head are not exported"):
        export_layouts(actor().state_dict(), spec, out, stage_dir)
    assert not list(out.iterdir())
    # The checkpoint alone says so too, whatever stage.json is beside it.
    (stage_dir / "stage.json").unlink()
    with pytest.raises(ValueError, match="the camera and its look head are not exported"):
        export_layouts(actor().state_dict(), spec, out, stage_dir)
