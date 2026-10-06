"""Free look and mixed render resolutions, the learner's half (camera-vision.FREELOOK.md, vision block revision 4,
protocol 22): the manifest's patch, render sizes and look heads; the encoder at 128 x 64 with patch 8; the look head's
draws, log probabilities and entropy, nothing for a row without the camera; the joint PPO ratio and the look's
entropy bonus; the rollout buffer's look; SPEC's LookHeads and ACT's look section on the wire; seeding and export."""

import copy
import dataclasses
import json
import socket
import struct
from types import SimpleNamespace

import numpy as np
import pytest
import torch
from torch.distributions import Categorical

from sim_threads import accept, joined, sim_thread  # noqa: E402
from animus import protocol as p
from animus.bootstrap import seed_trainer
from animus.env import ForgeEnv
from animus.export import export_layouts
from animus.mappo.buffer import RolloutBuffer
from animus.mappo.networks import (LOOK_HOLD_BIAS, LayoutActor, LookHead, VisionEncoder, check_look_heads,
                                   look_hold_indices, vision_look_heads, vision_of)
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout

H, W, PATCH, SCALARS, KINDS = 16, 32, 8, 11, 8
BYTES = H * W * 4
HEADS = (7, 5, 4)
LOOK_NAMES = ("yaw_rate", "pitch_rate", "zoom")
IMAGE = {"height": H, "width": W, "channels": 5, "kinds": KINDS, "kind_channel": 3, "scalars": SCALARS,
         "transport": "bytes", "bytes_per_pixel": 4, "patch": PATCH, "render_sizes": [[8, 4], [16, 8], [32, 16]]}
NAMES = ["warrior", "priest", "director"]
FIRST = {"warrior": 8, "priest": 12}


def block(name: str, first: int, count: int, actions=(0, 0), **extra) -> dict:
    return {"name": name, "obs": [first, count], "actions": list(actions), **extra}


def stage(look=True, revision: int = 4, image: dict | None = None) -> dict:
    """A revision 4 stage.json: two layouts with the camera (its 11 scalars) and its look, a director without."""
    layouts = {}
    for name, core in (("warrior", 5), ("priest", 9)):
        extra = {"look": {"heads": list(HEADS), "names": list(LOOK_NAMES)}} if look else {}
        blocks = [block("core", 0, core, (0, 3)), block("move", core, 3, (3, 2)),
                  block("vision", core + 3, SCALARS, revision=revision, image=dict(image or IMAGE), **extra)]
        layouts[name] = {"obs_dim": core + 3 + SCALARS, "blocks": blocks}
    layouts["director"] = {"obs_dim": 8, "blocks": [block("core", 0, 6, (0, 4))]}
    return {"stage": "test_look", "layouts": layouts}


def shapes(stage_json: dict) -> list[tuple[int, int]]:
    return [(stage_json["layouts"][name]["obs_dim"], 5 if name != "director" else 4) for name in NAMES]


def observations(rows: int, seed: int = 0, layouts=None):
    """(obs, layout, image) of every layout: random scalars and frames for the camera's, no frame for the director."""
    s = stage()
    widths = [s["layouts"][name]["obs_dim"] for name in NAMES]
    generator = torch.Generator().manual_seed(seed)
    layout = torch.tensor(layouts) if layouts is not None else torch.arange(rows) % len(NAMES)
    obs = torch.zeros(rows, max(widths))
    image = torch.tensor(p.NO_FRAME_PIXEL, dtype=torch.uint8).repeat(rows, H * W)
    for row in range(rows):
        index = int(layout[row])
        obs[row, : widths[index]] = torch.randn(widths[index], generator=generator)
        if NAMES[index] in FIRST:
            image[row] = torch.randint(0, 256, (BYTES,), generator=generator, dtype=torch.uint8)
            image[row, 3::4] &= 0x17
    return obs, layout, image


def trainer_of(config: MappoConfig | None = None, **kwargs) -> MappoTrainer:
    torch.manual_seed(0)
    return MappoTrainer(shapes(stage()), 4, config or MappoConfig(hidden=(16, 16), recurrent_size=4),
                        vision=vision_of(stage(), NAMES), **kwargs)


# ------------------------------------------------------------------ the manifest


def test_vision_of_reads_revision_4s_patch_render_sizes_and_look():
    vision = vision_of(stage(), NAMES)
    entry = vision[0]
    assert entry["patch"] == PATCH and entry["render_sizes"] == ((8, 4), (16, 8), (32, 16))
    assert entry["look"] == HEADS and entry["look_names"] == LOOK_NAMES and entry["scalars"] == 11
    assert vision[2] is None
    assert vision_look_heads(vision) == HEADS
    assert vision_look_heads(vision_of(stage(look=False), NAMES)) == ()
    assert vision_look_heads(None) == ()
    # The SPEC's LookHeads must be the manifest's head count.
    check_look_heads(vision, 3)
    check_look_heads(vision_of(stage(look=False), NAMES), 0)
    check_look_heads(None, 0)
    with pytest.raises(ValueError, match="look heads"):
        check_look_heads(vision, 0)
    with pytest.raises(ValueError, match="look heads"):
        check_look_heads(None, 3)
    # A render size larger than the canonical image is not one the sim can scale up from.
    with pytest.raises(ValueError, match="render size"):
        vision_of(stage(image={**IMAGE, "render_sizes": [[64, 16]]}), NAMES)


def test_the_encoder_at_128_x_64_with_patch_8_is_the_16_x_8_grid():
    image = {"height": 64, "width": 128, "channels": 5, "kinds": 8, "kind_channel": 3, "scalars": 11,
             "bytes_per_pixel": 4, "patch": 8, "image_bytes": 64 * 128 * 4}
    encoder = VisionEncoder([{"first": 3, **image}, None])
    assert encoder.PATCH == 8 and encoder.grid == (8, 16) and encoder.feature_shape == (64, 8, 16)
    # 8 x 8 pixels x 12 planes a patch; 128 keypoints and 11 scalars into the embedding.
    assert (encoder.patch.in_features, encoder.patch.out_features) == (768, 64)
    assert (encoder.mix.in_features, encoder.mix.out_features) == (64, 64)
    assert (encoder.embed.in_features, encoder.embed.out_features) == (139, 256)
    obs = torch.rand(3, 3 + 11)
    pixels = torch.randint(0, 256, (3, 64 * 128 * 4), dtype=torch.uint8)
    with torch.no_grad():
        embedding = encoder(obs, torch.tensor([0, 1, 0]), pixels)
        features = encoder.features(encoder.patches(encoder.planes(encoder.gather(obs, torch.tensor([0, 1, 0]),
                                                                                   pixels)[0])))
    assert embedding.shape == (3, 256) and features.shape == (3, 128, 64)
    # A manifest without a patch (revision 3) keeps 4 x 4.
    old = {**image, "height": 32, "width": 64, "scalars": 7, "image_bytes": 32 * 64 * 4}
    del old["patch"]
    assert VisionEncoder([{"first": 3, **old}]).PATCH == 4


# ------------------------------------------------------------------ the look head


def test_the_look_head_draws_in_range_its_log_prob_is_the_sum_of_three_and_its_entropy_too():
    torch.manual_seed(1)
    head = LookHead(16, HEADS, torch.tensor([True, True, False]))
    with torch.no_grad():
        head.linear.weight.normal_()
    features = torch.randn(300, 16)
    layout = torch.arange(300) % 3
    choice, log_prob = head.sample(features, layout, deterministic=False)
    assert choice.shape == (300, 3) and log_prob.shape == (300,)
    for index, size in enumerate(HEADS):
        assert int(choice[:, index].min()) >= 0 and int(choice[:, index].max()) < size
    logits = head.logits(features)
    dists = [Categorical(logits=part) for part in logits]
    expected = sum(dist.log_prob(choice[:, index]) for index, dist in enumerate(dists))
    seeing = layout != 2
    torch.testing.assert_close(log_prob[seeing], expected[seeing])
    evaluated, entropy = head.evaluate(features, layout, choice)
    torch.testing.assert_close(evaluated[seeing], expected[seeing])
    torch.testing.assert_close(entropy[seeing], sum(dist.entropy() for dist in dists)[seeing])
    # A row without the camera: choice 0 (what the wire carries), log probability and entropy 0.
    assert bool((choice[~seeing] == 0).all())
    assert bool((log_prob[~seeing] == 0).all()) and bool((evaluated[~seeing] == 0).all())
    assert bool((entropy[~seeing] == 0).all())
    # Deterministic is each head's argmax.
    greedy, _ = head.sample(features, layout, deterministic=True)
    for index, part in enumerate(logits):
        torch.testing.assert_close(greedy[seeing, index], part.argmax(-1)[seeing])


def test_a_row_without_the_camera_sends_no_gradient_through_the_look():
    head = LookHead(16, HEADS, torch.tensor([True, False]))
    features = torch.randn(4, 16)
    layout = torch.tensor([1, 1, 1, 1])
    log_prob, entropy = head.evaluate(features, layout, torch.ones(4, 3, dtype=torch.long))
    (log_prob.sum() + entropy.sum()).backward()
    assert head.linear.weight.grad is None or bool((head.linear.weight.grad == 0).all())


def test_a_fresh_look_head_leans_toward_holding_still_but_allows_everything():
    assert look_hold_indices(HEADS) == p.LOOK_HOLD == (3, 2, 0)
    head = LookHead(16, HEADS, torch.tensor([True]))
    with torch.no_grad():
        logits = head.logits(torch.zeros(1, 16))
    for part, hold in zip(logits, look_hold_indices(HEADS)):
        probs = torch.softmax(part[0], -1)
        assert int(probs.argmax()) == hold and float(part[0, hold]) == pytest.approx(LOOK_HOLD_BIAS)
        assert float(probs.min()) > 0.01        # nothing masked
    assert float(torch.softmax(logits[0][0], -1)[3]) == pytest.approx(np.exp(2) / (np.exp(2) + 6), rel=1e-4)


def test_an_actor_without_a_look_has_no_look_head():
    plain = vision_of(stage(look=False), NAMES)
    assert LayoutActor(shapes(stage()), [16, 16], vision=plain).look_head is None
    assert LayoutActor(shapes(stage()), [16, 16]).look_head is None
    actor = LayoutActor(shapes(stage()), [16, 16], vision=vision_of(stage(), NAMES))
    assert actor.look_head is not None and actor.look_head.heads == HEADS
    assert actor.look_head.linear.in_features == actor.head_width


# ------------------------------------------------------------------ acting and the update


def decide(trainer, envs=3, agents=2, seed=0, deterministic=False, state=None):
    obs, layout, image = observations(envs * agents, seed)
    arrays = (obs.numpy().reshape(envs, agents, -1), np.ones((envs, agents, 5), bool),
              layout.numpy().reshape(envs, agents),
              np.random.default_rng(seed).standard_normal((envs, 4)).astype(np.float32))
    state = state if state is not None else trainer.acting_state(envs, agents)
    out = trainer.act_and_value(*arrays, deterministic=deterministic, state=state,
                                image=image.numpy().reshape(envs, agents, -1))
    return arrays, image.numpy().reshape(envs, agents, -1), state, out


def test_a_decision_chooses_a_look_and_its_log_prob_is_joint():
    trainer = trainer_of()
    (obs, mask, layout, _), image, state, (actions, log_probs, *_) = decide(trainer)
    look = state.look
    assert look.shape == (3, 2, 3) and look.dtype == np.int8
    wire = trainer.wire_look(look)
    assert wire.dtype == np.int32 and wire.shape == (3, 2, 3)
    seeing = layout != 2
    assert bool((look[~seeing] == 0).all())
    # The joint log probability: the action's plus the three look heads'.
    actor = trainer._rollout_actor
    with torch.no_grad():
        rows = 6
        obs_t, layout_t = torch.as_tensor(obs).reshape(rows, -1), torch.as_tensor(layout, dtype=torch.long).reshape(-1)
        features = actor.features(obs_t, layout_t, torch.zeros(rows, 4), image=torch.as_tensor(image).reshape(rows, -1))
        action_lp = actor.action_distribution(features, layout_t, torch.as_tensor(mask).reshape(rows, -1)).log_prob(
            torch.as_tensor(actions).reshape(-1))
        look_lp, _ = actor.look_terms(features, layout_t, torch.as_tensor(look).reshape(rows, -1))
    np.testing.assert_allclose(log_probs.reshape(-1), (action_lp + look_lp).numpy(), rtol=1e-5, atol=1e-5)
    assert bool((look_lp.reshape(3, 2)[torch.as_tensor(~seeing)] == 0).all())
    # Deterministic: the argmax of every head, the same twice.
    first = decide(trainer, deterministic=True)[2].look
    second = decide(trainer, deterministic=True)[2].look
    np.testing.assert_array_equal(first, second)
    # act() (the evaluation's) sets the look too.
    acting = trainer.acting_state(3, 2)
    trainer.act(obs, mask, layout, True, acting, image)
    np.testing.assert_array_equal(acting.look, first)


def test_a_trainer_without_look_heads_sends_no_look():
    trainer = MappoTrainer(shapes(stage()), 4, MappoConfig(hidden=(16, 16), recurrent_size=4),
                           vision=vision_of(stage(look=False), NAMES))
    assert trainer.look_heads == () and trainer.acting_state(2, 2).look is None
    assert trainer.wire_look(None) is None


def fill(trainer, envs=3, agents=2, steps=4):
    buffer = RolloutBuffer(steps, envs, agents, max(o for o, _ in shapes(stage())), 4, 5, 0, trainer.recurrent_size,
                           image_bytes=BYTES, look_heads=len(trainer.look_heads))
    acting = trainer.acting_state(envs, agents)
    for step in range(steps):
        memory, critic_memory = acting.memory.copy(), acting.critic_memory.copy()
        (o, mask, l, st), im, acting, (chosen, log_probs, values, _, goals, _) = decide(trainer, envs, agents, step,
                                                                                         state=acting)
        buffer.add_decision(o, st, mask, l, chosen, log_probs, values, None, None, memory, goals, critic_memory,
                            image=im, look=acting.look)
        np.testing.assert_array_equal(buffer.look[step], acting.look)
        buffer.add_outcome(np.random.default_rng(step).standard_normal((envs, agents)).astype(np.float32),
                           np.zeros(envs, bool), np.zeros(envs, bool), np.zeros((envs, agents), np.float32))
    buffer.finish(np.zeros((envs, agents), np.float32), 0.99, 0.95)
    return buffer


def test_the_rollout_buffer_holds_the_look():
    trainer = trainer_of()
    buffer = fill(trainer)
    assert buffer.look.dtype == np.int8 and buffer.look.shape == (4, 3, 2, 3)
    assert "look" in buffer.sequences()
    assert int(buffer.look[..., 0].max()) < 7 and int(buffer.look[..., 2].max()) < 4
    # A buffer without look heads keeps none, and its sequences do not carry one.
    plain = RolloutBuffer(2, 1, 1, 3, 2, 2)
    assert plain.look.shape == (2, 1, 1, 0) and "look" not in plain.sequences()
    # An exploiter's view shares the main's look (what was sent), as it shares the actions.
    view = RolloutBuffer.view_of(buffer, 0, trainer.recurrent_size, False, 0, 1)
    assert view.look is buffer.look and view.look_heads == 3


def test_the_update_trains_the_look_head_with_its_entropy_and_says_whether_it_turns():
    trainer = trainer_of(MappoConfig(hidden=(16, 16), recurrent_size=4, epochs=1, minibatches=1))
    buffer = fill(trainer)
    before = trainer.actor.look_head.linear.weight.detach().clone()
    stats = trainer.update(buffer)
    assert not torch.equal(before, trainer.actor.look_head.linear.weight)
    assert stats["look_entropy"] > 0.0
    # At most ln 7 + ln 5 + ln 4 nats a camera row.
    assert stats["look_entropy"] <= np.log(7 * 5 * 4) + 1e-4
    seeing = buffer.layout != 2
    turning = float((buffer.look[..., 0][seeing] != 3).mean())
    assert stats["look_turning"] == pytest.approx(turning, abs=1e-6)
    assert {"look_pitching", "look_zooming"} <= set(stats)
    # The rollout copy reads the update's look head.
    torch.testing.assert_close(trainer._rollout_actor.look_head.linear.weight, trainer.actor.look_head.linear.weight)


def test_the_look_entropy_coefficient_follows_entropy_coef():
    trainer = trainer_of(MappoConfig(hidden=(16, 16), recurrent_size=4, entropy_coef=0.02))
    assert trainer.look_entropy_coef() == pytest.approx(0.02)
    trainer.entropy_coef = 0.01                       # the schedule or the floor moved it
    assert trainer.look_entropy_coef() == pytest.approx(0.01)
    explicit = trainer_of(MappoConfig(hidden=(16, 16), recurrent_size=4, entropy_coef=0.02, look_entropy_coef=0.005))
    assert explicit.look_entropy_coef() == pytest.approx(0.005)
    explicit.entropy_coef = 0.01
    assert explicit.look_entropy_coef() == pytest.approx(0.0025)


def test_the_ratio_is_one_on_the_first_epoch_with_the_look_in_it():
    """The stored log probability is the joint one, so the update's first pass reads a ratio of 1 (approx KL 0)."""
    trainer = trainer_of(MappoConfig(hidden=(16, 16), recurrent_size=4, epochs=1, minibatches=1, actor_lr=0.0,
                                     critic_lr=0.0, normalise_observations=False))
    stats = trainer.update(fill(trainer))
    assert stats["approx_kl"] == pytest.approx(0.0, abs=1e-6) and stats["clip_frac"] == 0.0


def test_the_camera_update_in_chunks_is_the_whole_minibatchs():
    """mappo.vision_chunk_rows encodes the minibatch a chunk at a time and again for the gradient: the same step."""
    steps = []
    for chunk_rows in (0, 5):
        trainer = trainer_of(MappoConfig(hidden=(16, 16), recurrent_size=4, epochs=1, minibatches=1,
                                         vision_chunk_rows=chunk_rows))
        torch.manual_seed(3)
        np.random.seed(3)
        buffer = fill(trainer)
        torch.manual_seed(4)
        stats = trainer.update(buffer)
        steps.append((stats["vision_grad_norm"], [p.detach().clone() for p in trainer.actor.vision.parameters()]))
    assert steps[0][0] == pytest.approx(steps[1][0], rel=1e-4)
    for whole, chunked in zip(steps[0][1], steps[1][1]):
        torch.testing.assert_close(whole, chunked, rtol=1e-4, atol=1e-6)


@pytest.mark.skipif(not torch.cuda.is_available(), reason="rollout graphs need a GPU")
def test_a_rollout_graph_captures_the_look_head():
    torch.manual_seed(0)
    trainer = MappoTrainer(shapes(stage()), 4, MappoConfig(hidden=(16, 16), recurrent_size=8), train_device="cuda",
                           rollout_device="cuda", vision=vision_of(stage(), NAMES))
    with torch.no_grad():
        trainer.actor.look_head.linear.weight.normal_()
    trainer.sync_rollout()
    eager_state = trainer.acting_state(3, 2)
    graph_state = copy.deepcopy(eager_state)
    trainer.config.rollout_graphs = False
    _, _, _, eager = decide(trainer, deterministic=True, state=eager_state)
    trainer.config.rollout_graphs = True
    _, _, _, graphed = decide(trainer, deterministic=True, state=graph_state)
    assert trainer._rollout_graphs, "the decision was not captured"
    np.testing.assert_array_equal(eager_state.look, graph_state.look)
    np.testing.assert_allclose(eager[1], graphed[1], rtol=1e-4, atol=1e-4)      # joint log probabilities
    # Sampled replays draw fresh looks, in range.
    looks = [decide(trainer, seed=0, state=copy.deepcopy(eager_state))[2].look for _ in range(4)]
    assert all(int(look[..., 0].max()) < 7 for look in looks)


# ------------------------------------------------------------------ the wire


SPEC = p.Spec(version=p.PROTOCOL_VERSION, num_envs=3, agents_per_env=2, obs_dim=4, state_dim=5, num_actions=3,
              episode_info_dim=2, goal_count=0, tick_ms=50, decision_ticks=1, episode_seconds=60, scenario="looking",
              layouts=(p.Layout("warrior", 4, 3),), episode_info_names=("damage",), kinematics_dim=10,
              image_bytes=16, look_heads=3)
PLAIN = dataclasses.replace(SPEC, image_bytes=0, look_heads=0)


def test_spec_is_92_bytes_ending_with_the_look_heads():
    assert p.PROTOCOL_VERSION == 22
    assert p.SPEC.format == "<12I32s3I" and p.SPEC.size == 92
    payload = p.encode_spec(SPEC)
    assert payload[84:92] == struct.pack("<II", 16, 3)
    assert p.decode_spec(payload) == SPEC and p.decode_spec(p.encode_spec(PLAIN)).look_heads == 0


def test_act_round_trips_with_the_look_agent_major_after_the_goals():
    rng = np.random.default_rng(0)
    actions = rng.integers(0, 3, (2, 3)).astype(np.int32)
    goals = rng.integers(-1, 4, (2, 3, 2)).astype(np.int32)
    look = np.stack([rng.integers(0, h, (2, 3)) for h in HEADS], axis=-1).astype(np.int32)
    payload = p.encode_act(5, actions, goals, look)
    # As Protocol.h lays it out: header, int32 actions[E*A], int32 goals[E*A*2], int32 look[E*A*3].
    expected = struct.pack("<II", 5, 2) + actions.astype("<i4").tobytes() + goals.astype("<i4").tobytes()
    expected += b"".join(struct.pack("<3i", *look[e, a]) for e in range(2) for a in range(3))
    assert payload == expected
    begin, got_actions, got_goals, got_look = p.decode_act(payload, 3, goals=True, look_heads=3)
    assert begin == 5
    np.testing.assert_array_equal(got_actions, actions)
    np.testing.assert_array_equal(got_goals, goals)
    np.testing.assert_array_equal(got_look, look)
    # Without goals the look follows the actions directly.
    begin, _, none, alone = p.decode_act(p.encode_act(0, actions, None, look), 3, look_heads=3)
    assert none is None
    np.testing.assert_array_equal(alone, look)
    with pytest.raises(ValueError):
        p.decode_act(p.encode_act(0, actions, None, look), 3, look_heads=0)


def test_a_stage_without_look_heads_sends_protocol_21s_act():
    actions = np.arange(6, dtype=np.int32).reshape(3, 2)
    protocol_21 = struct.pack("<II", 0, 3) + actions.astype("<i4").tobytes()
    assert p.encode_act(0, actions) == protocol_21
    sent = []
    env = ForgeEnv.__new__(ForgeEnv)
    env.spec = PLAIN
    env.sock = SimpleNamespace(sendall=sent.append)
    # Whatever look a caller has, a stage without look heads sends none.
    env.send_act(0, actions, None, np.ones((3, 2, 3), np.int32))
    assert sent == [p.encode_header(p.MsgType.ACT, len(protocol_21)) + protocol_21]
    # A stage with look heads always sends one: the hold look where the caller has none.
    env.spec = SPEC
    env.send_act(0, actions)
    _, _, _, look = p.decode_act(sent[-1][p.HEADER.size:], 2, look_heads=3)
    assert bool((look == np.array(p.LOOK_HOLD)).all())
    with pytest.raises(ValueError, match="look must have shape"):
        env.send_act(0, actions, None, np.zeros((3, 2, 2), np.int32))


def read_exact(conn: socket.socket, size: int) -> bytes:
    data = b""
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        assert chunk, "the learner hung up"
        data += chunk
    return data


def test_a_sim_with_look_heads_over_the_socket(tmp_path):
    """SPEC with LookHeads 3, a STEP, and the ACT that comes back carrying every agent's look."""
    path = str(tmp_path / "looking.sock")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(1)
    rng = np.random.default_rng(2)
    step = p.Step(decision=0, obs=rng.standard_normal((3, 2, 4)).astype(np.float32),
                  state=np.zeros((3, 5), np.float32), mask=np.ones((3, 2, 3), bool),
                  layout=np.zeros((3, 2), np.uint16), present=np.ones((3, 2), bool),
                  reward=np.zeros((3, 2), np.float32), done=np.zeros(3, bool), terminated=np.zeros(3, bool),
                  final_obs=np.zeros((3, 2, 4), np.float32), final_state=np.zeros((3, 5), np.float32),
                  episode_info=np.zeros((3, 2, 2), np.float32), episode_seed=np.zeros(3, np.uint32),
                  image=p.no_frame((3, 2, 16)), final_image=p.no_frame((3, 2, 16)))
    received = []

    def sim():
        conn = accept(listener)
        with conn:
            _, length = p.HEADER.unpack(read_exact(conn, p.HEADER.size))
            assert p.HELLO.unpack(read_exact(conn, length))[0] == 22
            spec = p.encode_spec(SPEC)
            conn.sendall(p.encode_header(p.MsgType.SPEC, len(spec)) + spec)
            payload = p.encode_header(p.MsgType.STEP, len(p.encode_step(SPEC, step))) + p.encode_step(SPEC, step)
            conn.sendall(payload)
            for _ in range(2):
                msg_type, length = p.HEADER.unpack(read_exact(conn, p.HEADER.size))
                assert msg_type == p.MsgType.ACT
                received.append(p.decode_act(read_exact(conn, length), 2, look_heads=3))
                conn.sendall(payload)
            read_exact(conn, p.HEADER.size)

    server = sim_thread(sim)
    server.start()
    env = ForgeEnv(path, connect_timeout=5.0)
    assert env.spec.look_heads == 3
    env.reset()
    look = np.stack([rng.integers(0, h, (3, 2)) for h in HEADS], axis=-1).astype(np.int32)
    env.step(np.zeros((3, 2), np.int32), None, look)
    env.step(np.zeros((3, 2), np.int32))
    env.close()
    joined(server, 5)
    listener.close()
    np.testing.assert_array_equal(received[0][3], look)
    assert bool((received[1][3] == np.array(p.LOOK_HOLD)).all())


def test_a_sim_speaking_protocol_21_is_refused(tmp_path):
    path = str(tmp_path / "old.sock")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(1)

    def sim():
        conn = accept(listener)
        with conn:
            _, length = p.HEADER.unpack(read_exact(conn, p.HEADER.size))
            read_exact(conn, length)
            spec = p.encode_spec(dataclasses.replace(SPEC, version=21))
            conn.sendall(p.encode_header(p.MsgType.SPEC, len(spec)) + spec)

    server = sim_thread(sim)
    server.start()
    with pytest.raises(ConnectionError, match="protocol 21"):
        ForgeEnv(path, connect_timeout=5.0)
    joined(server, 5)
    listener.close()


# ------------------------------------------------------------------ seeding and export


def test_seeding_starts_the_look_head_fresh_or_carries_it():
    config = MappoConfig(hidden=(16, 16))
    layouts = [Layout(name, obs, actions) for name, (obs, actions) in zip(NAMES, shapes(stage()))]
    spec = SimpleNamespace(layouts=tuple(layouts), state_dim=4)
    checkpoint_spec = {"layouts": [{"name": l.name, "obs_dim": l.obs_dim, "num_actions": l.num_actions}
                                   for l in layouts]}
    old = trainer_of(config)
    with torch.no_grad():
        old.actor.look_head.linear.weight.fill_(0.5)
    checkpoint = {"trainer": old.state_dict(), "spec": checkpoint_spec, "stage": stage()}

    carried = trainer_of(config)
    seed_trainer(carried, checkpoint, spec, stage())
    torch.testing.assert_close(carried.actor.look_head.linear.weight, old.actor.look_head.linear.weight)

    # From a revision 3 camera without a look (the stage before this change): the look head and the encoder fresh,
    # the join zeroed, so the seeded policy starts as it was.
    torch.manual_seed(5)
    before = MappoTrainer(shapes(stage()), 4, config, vision=vision_of(stage(look=False, revision=3), NAMES))
    old_checkpoint = {"trainer": before.state_dict(), "spec": checkpoint_spec, "stage": stage(look=False, revision=3)}
    fresh = trainer_of(config)
    initial = fresh.actor.look_head.linear.bias.detach().clone()
    seed_trainer(fresh, old_checkpoint, spec, stage())
    torch.testing.assert_close(fresh.actor.look_head.linear.bias, initial)
    assert bool((fresh.actor.vision_join.linear.weight == 0).all())


def test_export_says_plainly_that_the_look_head_is_not_exported(tmp_path):
    s = stage()
    stage_dir = tmp_path / "layouts" / "test_look"
    stage_dir.mkdir(parents=True)
    (stage_dir / "stage.json").write_text(json.dumps(s))
    spec = {"scenario": "test_look",
            "layouts": [{"name": name, "obs_dim": obs, "num_actions": actions}
                        for name, (obs, actions) in zip(NAMES, shapes(s))]}
    out = tmp_path / "models"
    out.mkdir()
    state = trainer_of().actor.state_dict()
    assert any(key.startswith("look_head.") for key in state)
    with pytest.raises(ValueError, match="the camera and its look head are not exported"):
        export_layouts(state, spec, out, stage_dir)
    assert not list(out.iterdir())
