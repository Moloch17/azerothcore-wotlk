"""The camera's encoder (camera-vision, naive slice): stage.json's vision block read per layout, the image laid out
[row][col][channel], the kind one-hot, the columns kept from the adapters and their normalisers, the zero join, the
rollout graph kept on, export refused, and the networks of a stage without a camera exactly as they were."""

import copy
import json
from types import SimpleNamespace

import numpy as np
import pytest
import torch

from animus.bootstrap import seed_trainer
from animus.export import export_layouts
from animus.mappo.networks import (LayoutActor, LayoutCritic, VisionEncoder, update_norms, vision_of,
                                   without_blind_columns)
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout

H, W, C, KINDS, KIND_CHANNEL, SCALARS = 6, 8, 5, 8, 3, 7
SPAN = H * W * C + SCALARS
IMAGE = {"height": H, "width": W, "channels": C, "kinds": KINDS, "kind_channel": KIND_CHANNEL, "scalars": SCALARS}
# Two layouts with a camera at different columns (their core blocks differ in width, as the classes' do) and one
# without.
NAMES = ["warrior", "priest", "director"]
FIRST = {"warrior": 8, "priest": 12}


def block(name: str, first: int, count: int, actions=(0, 0), **extra) -> dict:
    return {"name": name, "obs": [first, count], "actions": list(actions), **extra}


def stage(image: dict | None = IMAGE, vision_revision: int = 1) -> dict:
    """A stage.json: warrior core 5 + move 3, priest core 9 + move 3, then the vision block and a goal block of 2;
    the director core 6 + goal 2."""
    layouts = {}
    for name, core in (("warrior", 5), ("priest", 9)):
        blocks = [block("core", 0, core, (0, 3)), block("move", core, 3, (3, 2))]
        at = core + 3
        if image is not None:
            blocks.append(block("vision", at, SPAN, revision=vision_revision, image=image))
            at += SPAN
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


def observations(rows: int, seed: int = 0, layouts=None) -> tuple[torch.Tensor, torch.Tensor]:
    """Random rows of every layout, the camera's columns a plausible image (kinds 0-7, the rest in [0, 1])."""
    s = stage()
    widths = [s["layouts"][name]["obs_dim"] for name in NAMES]
    generator = torch.Generator().manual_seed(seed)
    layout = (torch.tensor(layouts) if layouts is not None
              else torch.arange(rows) % len(NAMES))
    obs = torch.zeros(rows, max(widths))
    for row in range(rows):
        index = int(layout[row])
        obs[row, : widths[index]] = torch.randn(widths[index], generator=generator)
        if NAMES[index] in FIRST:
            first = FIRST[NAMES[index]]
            image = torch.rand(H, W, C, generator=generator)
            image[..., KIND_CHANNEL] = torch.randint(0, KINDS, (H, W), generator=generator).float()
            obs[row, first : first + H * W * C] = image.reshape(-1)
            obs[row, first + H * W * C : first + SPAN] = torch.rand(SCALARS, generator=generator)
    return obs, layout


def with_image(obs: torch.Tensor, layout: torch.Tensor, value: float | None) -> torch.Tensor:
    """`obs` with every camera column set to `value` (None: fresh noise)."""
    out = obs.clone()
    for row in range(obs.shape[0]):
        name = NAMES[int(layout[row])]
        if name in FIRST:
            columns = slice(FIRST[name], FIRST[name] + SPAN)
            out[row, columns] = torch.rand(SPAN) * 7 if value is None else value
    return out


# ------------------------------------------------------------------ stage.json


def test_vision_of_keeps_each_layouts_own_first_column():
    vision = vision_of(stage(), NAMES)
    assert [entry and entry["first"] for entry in vision] == [8, 12, None]
    assert vision[0] == {"first": 8, **IMAGE}


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
    with pytest.raises(ValueError, match="image and scalars"):
        vision_of(short, NAMES)
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


def test_the_image_is_read_row_col_channel_from_each_layouts_own_columns():
    encoder = VisionEncoder(vision_of(stage(), NAMES), 16)
    obs = torch.zeros(2, 12 + SPAN + 2)
    layout = torch.tensor([0, 1])
    row, col, channel = 4, 6, 2
    for index, name in enumerate(("warrior", "priest")):
        first = FIRST[name]
        obs[index, first + (row * W + col) * C + channel] = 0.25 + index
        obs[index, first + H * W * C + 5] = 0.5 + index         # the sixth scalar (underwater)
    image, scalars = encoder.gather(obs, layout)
    assert image.shape == (2, C, H, W) and scalars.shape == (2, SCALARS)
    for index in range(2):
        assert float(image[index, channel, row, col]) == 0.25 + index
        assert float(image[index].abs().sum()) == 0.25 + index      # nothing else: the layout's own offset
        assert float(scalars[index, 5]) == 0.5 + index


def test_the_kind_channel_becomes_a_one_hot_of_the_kinds():
    encoder = VisionEncoder(vision_of(stage(), NAMES), 16)
    image = torch.zeros(1, C, H, W)
    image[0, 0] = 0.7                                   # distance
    image[0, 4] = 1.0                                   # objective
    kinds = torch.tensor([0.0, 1.0, 2.6, 3.4, -1.0, 9.0, 6.0, 7.0])   # rounded, clamped to 0..7
    image[0, KIND_CHANNEL, 0, :] = kinds
    planes = encoder.planes(image)
    assert planes.shape == (1, C - 1 + KINDS, H, W) == (1, 12, H, W)
    # The other channels in order (distance, height, normal, objective), then the one-hot.
    torch.testing.assert_close(planes[0, 0], image[0, 0])
    torch.testing.assert_close(planes[0, 3], image[0, 4])
    expected = [0, 1, 3, 3, 0, 7, 6, 7]
    for col, kind in enumerate(expected):
        hot = planes[0, 4:, 0, col]
        assert float(hot.sum()) == 1.0 and int(hot.argmax()) == kind
    assert bool((planes[0, 4, 1:] == 1.0).all())        # the rest of the image is kind 0 (sky)


def test_shapes_at_the_default_camera():
    image = {"height": 32, "width": 64, "channels": 5, "kinds": 8, "kind_channel": 3, "scalars": 7}
    span = 32 * 64 * 5 + 7
    assert span == 10_247
    descriptors = [{"first": 3, **image}, None]
    encoder = VisionEncoder(descriptors, 24)
    # Padding 1: 32 x 64 -> 16 x 32 -> 8 x 16 -> 8 x 16, 64 channels.
    assert encoder.feature_shape == (64, 8, 16)
    sizes = []
    x = torch.zeros(1, 12, 32, 64)
    for layer in encoder.convs:
        x = layer(x)
        if isinstance(layer, torch.nn.Conv2d):
            sizes.append(tuple(x.shape[1:]))
    assert sizes == [(32, 16, 32), (64, 8, 16), (64, 8, 16)]
    assert encoder.embed.in_features == 128 + 7 and encoder.embed.out_features == 256
    assert encoder.join.in_features == 256 and encoder.join.out_features == 24
    obs = torch.rand(5, 3 + span)
    layout = torch.tensor([0, 1, 0, 1, 0])
    with torch.no_grad():
        encoder.join.weight.normal_()
        encoder.join.bias.normal_()
    out = encoder(obs, layout)
    assert out.shape == (5, 24)
    assert bool((out[1] == 0).all()) and bool((out[3] == 0).all())     # no camera, nothing added
    assert bool((out[0] != 0).any())
    with torch.no_grad():
        points = encoder.keypoints(encoder.convs(encoder.planes(encoder.gather(obs, layout)[0])))
    assert points.shape == (5, 128) and float(points.abs().max()) <= 1.0


# ------------------------------------------------------------------ the networks


def test_the_camera_columns_are_blind_to_the_adapters():
    net = actor()
    vision = vision_of(stage(), NAMES)
    obs, layout = observations(9)
    with torch.no_grad():
        net.vision.join.weight.normal_()               # the camera in the logits, so gradient reaches everything
    logits = net(obs, layout, torch.ones(9, 5)).logits
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
    assert net.vision.convs[0].weight.grad is not None and bool((net.vision.convs[0].weight.grad != 0).any())


def test_the_camera_columns_keep_an_identity_normaliser():
    net = actor()
    obs, layout = observations(30, seed=4)
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
    obs, layout = observations(12)
    mask = torch.ones(12, 5)
    with torch.no_grad():
        dark = net(with_image(obs, layout, 0.0), layout, mask).logits
        lit = net(with_image(obs, layout, 1.0), layout, mask).logits
    assert not torch.allclose(dark, lit)
    net(with_image(obs, layout, 0.5), layout, mask).logits.sum().backward()
    assert bool((net.vision.join.weight.grad != 0).any())
    assert bool((net.vision.convs[0].weight.grad != 0).any())
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
    assert bool((seeded.actor.vision.join.weight == 0).all()) and bool((seeded.actor.vision.join.bias == 0).all())
    assert bool((seeded.critic.vision.join.weight == 0).all())
    obs, layout = observations(12)
    mask = torch.ones(12, 5)
    with torch.no_grad():
        torch.testing.assert_close(seeded.actor(with_image(obs, layout, 1.0), layout, mask).logits,
                                   seeded.actor(with_image(obs, layout, 0.0), layout, mask).logits)


def test_the_critic_has_its_own_camera():
    trainer = MappoTrainer(shapes(stage()), 4, MappoConfig(hidden=(16, 16)), vision=vision_of(stage(), NAMES))
    assert trainer.actor.vision is not None and trainer.critic.vision is not None
    assert trainer.actor.vision.convs[0].weight is not trainer.critic.vision.convs[0].weight
    obs, layout = observations(6)
    state = torch.randn(6, 4)
    with torch.no_grad():
        value = trainer.critic(state, obs, layout)
        trainer.critic.vision.join.weight.normal_()
        assert not torch.allclose(trainer.critic(state, obs, layout), value)


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
            network.vision.join.weight.normal_(std=0.5)
    trainer.sync_rollout()
    obs, layout = observations(6, layouts=[0, 1, 2, 0, 1, 2])
    arrays = (obs.numpy().reshape(3, 2, -1), np.ones((3, 2, 5), bool), layout.numpy().reshape(3, 2),
              np.random.default_rng(0).standard_normal((3, 4)).astype(np.float32))
    eager_state = trainer.acting_state(3, 2)
    graph_state = copy.deepcopy(eager_state)
    trainer.config.rollout_graphs = False
    eager = trainer.act_and_value(*arrays, deterministic=True, state=eager_state)
    trainer.config.rollout_graphs = True
    graphed = trainer.act_and_value(*arrays, deterministic=True, state=graph_state)
    assert trainer._rollout_graphs, "the decision was not captured"
    np.testing.assert_allclose(eager[2], graphed[2], rtol=1e-4, atol=1e-5)          # values
    np.testing.assert_allclose(eager_state.memory, graph_state.memory, rtol=1e-4, atol=1e-5)
    # The graph read the camera: another image, other values.
    blank = (with_image(obs, layout, 0.0).numpy().reshape(3, 2, -1), *arrays[1:])
    other = trainer.act_and_value(*blank, deterministic=True, state=copy.deepcopy(eager_state))
    assert not np.allclose(other[2], graphed[2])


# ------------------------------------------------------------------ checkpoints and export


def test_a_checkpoint_round_trips_with_the_camera():
    vision = vision_of(stage(), NAMES)
    config = MappoConfig(hidden=(16, 16), recurrent_size=4)
    torch.manual_seed(0)
    saved = MappoTrainer(shapes(stage()), 4, config, vision=vision)
    with torch.no_grad():
        saved.actor.vision.join.weight.normal_()
        saved.critic.vision.embed.bias.normal_()
    state = saved.state_dict()
    assert any(key.startswith("vision.") for key in state["actor"])
    assert any(key.startswith("vision_keep_") for key in state["actor"])
    assert not any(key.startswith("vision_keep_") for key in without_blind_columns(state["actor"]))
    torch.manual_seed(1)
    loaded = MappoTrainer(shapes(stage()), 4, config, vision=vision)
    loaded.load_state_dict(state)
    for network in ("actor", "critic"):
        for key, value in getattr(saved, network).state_dict().items():
            torch.testing.assert_close(getattr(loaded, network).state_dict()[key], value)
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
        old.actor.vision.join.weight.fill_(0.5)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec, "stage": stage()}

    carried = MappoTrainer(shapes(stage()), 4, config, vision=vision_of(stage(), NAMES))
    seed_trainer(carried, checkpoint, spec, stage())
    torch.testing.assert_close(carried.actor.vision.join.weight, old.actor.vision.join.weight)
    assert carried.director_columns_clear()

    revised = MappoTrainer(shapes(stage()), 4, config, vision=vision_of(stage(), NAMES))
    seed_trainer(revised, checkpoint, spec, stage(vision_revision=2))
    assert bool((revised.actor.vision.join.weight == 0).all())
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
    with pytest.raises(ValueError, match="vision layers are not in the realm format yet"):
        export_layouts(actor().state_dict(), spec, out, stage_dir)
    assert not list(out.iterdir())
    # The checkpoint alone says so too, whatever stage.json is beside it.
    (stage_dir / "stage.json").unlink()
    with pytest.raises(ValueError, match="vision layers are not in the realm format yet"):
        export_layouts(actor().state_dict(), spec, out, stage_dir)
