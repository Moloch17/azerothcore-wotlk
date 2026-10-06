"""The camera's frames as bytes (protocol 21, camera-vision.BYTES.md; five bytes a pixel since protocol 23,
perception-goals P2), against the sim's C++ as it writes them:
SpecMsg's ImageBytes, the STEP's image and final_image sections (only with a camera), the DEVICE message's image handle,
the exact decode table (Vision::DecodePixel), the no-frame pattern, the stage.json cross-check, and a stage without a
camera on the wire exactly as protocol 20 (apart from SPEC's four bytes)."""

import dataclasses
import socket
import struct

import numpy as np
import pytest
import torch

from sim_threads import accept, joined, sim_thread  # noqa: E402
from animus import protocol as p
from animus.env import ForgeEnv
from animus.mappo.buffer import RolloutBuffer
from animus.mappo.networks import check_image_bytes, decode_image, decode_slots, vision_image_bytes

H, W = 4, 8
I = H * W * 5
SPEC = p.Spec(version=p.PROTOCOL_VERSION, num_envs=3, agents_per_env=2, obs_dim=4, state_dim=5, num_actions=3,
              episode_info_dim=2, goal_count=0, tick_ms=50, decision_ticks=1, episode_seconds=60, scenario="seeing",
              layouts=(p.Layout("warrior", 4, 3),), episode_info_names=("damage", "dps"), kinematics_dim=10,
              image_bytes=I)
PLAIN = dataclasses.replace(SPEC, image_bytes=0)

# Vision/Camera.h and VisionCaster.cpp (revision 5, perception-goals P2), ported as written.
NEAR, DISTANCE_REFERENCE, DISTANCE_LEVELS = 0.25, 1000.0, 254.0
SKY_BYTE, HEIGHT_ZERO, HEIGHT_STEP, HEIGHT_LIMIT = 255, 128, 0.2, 125
CLASS_MASK, OBJECTIVE_BIT = 0x1F, 0x20
SKY = 0
CLASSES, SLOTS = 23, 32


def cpp_round(value: float) -> int:
    """std::lround: halves away from zero."""
    return int(np.sign(value) * np.floor(abs(value) + 0.5))


def encode_pixel(kind: int, distance: float, rise: float, normal: float, objective: bool, slot: int = 0) -> list[int]:
    """Vision::EncodePixel, in float32 as the C++ computes it (`kind` is the class)."""
    f = np.float32
    sky = kind == SKY
    scaled = np.clip(f(np.log(f(max(distance, NEAR)) / f(NEAR))) / f(np.log(f(DISTANCE_REFERENCE) / f(NEAR))), 0, 1)
    steps = int(np.clip(cpp_round(f(rise) / f(HEIGHT_STEP)), -HEIGHT_LIMIT, HEIGHT_LIMIT))
    return [SKY_BYTE if sky else cpp_round(f(DISTANCE_LEVELS) * f(scaled)),
            HEIGHT_ZERO if sky else HEIGHT_ZERO + steps,
            cpp_round(f(255.0) * f(np.clip(normal, 0.0, 1.0))),
            (kind & CLASS_MASK) | (OBJECTIVE_BIT if objective else 0),
            slot]


def decode_pixel(pixel) -> list[float]:
    """Vision::DecodePixel, in float32."""
    f = np.float32
    return [f(1.0) if pixel[0] == SKY_BYTE else f(pixel[0]) / f(DISTANCE_LEVELS),
            f(int(pixel[1]) - HEIGHT_ZERO) / f(HEIGHT_LIMIT),
            f(pixel[2]) / f(255.0),
            f(pixel[3] & CLASS_MASK),
            f((pixel[3] >> 5) & 1)]


def cpp_spec_bytes(spec: p.Spec) -> bytes:
    """SpecMsg as Protocol.h packs it -- twelve u32, the 32-byte scenario name, u32 KinematicsDim, u32 ImageBytes --
    then the layouts and the episode info names."""
    body = struct.pack("<12I", spec.version, spec.num_envs, spec.agents_per_env, spec.obs_dim, spec.state_dim,
                       spec.num_actions, spec.episode_info_dim, spec.goal_count, spec.tick_ms, spec.decision_ticks,
                       spec.episode_seconds, spec.env_groups)
    body += spec.scenario.encode("ascii").ljust(32, b"\0") + struct.pack("<III", spec.kinematics_dim, spec.image_bytes,
                                                                          spec.look_heads)
    body += struct.pack("<I", len(spec.layouts))
    for layout in spec.layouts:
        body += struct.pack("<II", layout.obs_dim, layout.num_actions) + layout.name.encode("ascii").ljust(48, b"\0")
    return body + ",".join(spec.episode_info_names).encode("ascii")


def cpp_step_bytes(spec: p.Spec, step: dict, done: np.ndarray, device: bool = False) -> bytes:
    """A STEP as the sim writes it (Protocol.h's STEP, in its order): the header, then the protocol 20 fields, then --
    only with ImageBytes > 0 -- u8 image[E*A*I] (left out with device buffers) and u8 final_image[D*A*I]."""
    e = len(done)
    out = struct.pack("<QII", 7, 0, e)
    if not device:
        out += step["obs"].astype("<f4").tobytes() + step["state"].astype("<f4").tobytes()
        out += step["mask"].astype("u1").tobytes()
    out += step["layout"].astype("<u2").tobytes() + step["present"].astype("u1").tobytes()
    out += step["reward"].astype("<f4").tobytes() + done.astype("u1").tobytes() + done.astype("u1").tobytes()
    out += step["obs"][done].astype("<f4").tobytes() + step["state"][done].astype("<f4").tobytes()
    out += step["info"][done].astype("<f4").tobytes() + np.full(e, p.NO_EPISODE_SEED, "<u4").tobytes()
    out += step["kinematics"].astype("<f4").tobytes()
    if spec.image_bytes:
        if not device:
            out += step["image"].astype("u1").tobytes()
        out += step["final_image"][done].astype("u1").tobytes()
    return out


def fields(spec: p.Spec, rng: np.random.Generator) -> dict:
    e, a = spec.num_envs, spec.agents_per_env
    out = {"obs": rng.standard_normal((e, a, spec.obs_dim)), "state": rng.standard_normal((e, spec.state_dim)),
           "mask": rng.random((e, a, spec.num_actions)) < 0.5, "layout": np.zeros((e, a)),
           "present": np.ones((e, a)), "reward": rng.standard_normal((e, a)),
           "info": rng.standard_normal((e, a, spec.episode_info_dim)),
           "kinematics": rng.standard_normal((e, a, spec.kinematics_dim))}
    if spec.image_bytes:
        # Every byte value somewhere, 0-255 (a u1 field read as bool would turn them all into 0 and 1).
        out["image"] = rng.integers(0, 256, (e, a, spec.image_bytes), dtype=np.uint8)
        out["final_image"] = rng.integers(0, 256, (e, a, spec.image_bytes), dtype=np.uint8)
    return out


# ------------------------------------------------------------------ SPEC


def test_spec_carries_the_image_bytes_as_the_sim_packs_them():
    assert p.SPEC.format == "<12I32s3I" and p.SPEC.size == 92
    for spec in (SPEC, PLAIN):
        payload = cpp_spec_bytes(spec)
        assert p.encode_spec(spec) == payload
        assert p.decode_spec(payload) == spec
    assert p.decode_spec(cpp_spec_bytes(PLAIN)).image_bytes == 0


# ------------------------------------------------------------------ STEP


def test_a_step_with_a_camera_reads_its_images_as_the_sim_writes_them():
    rng = np.random.default_rng(3)
    data = fields(SPEC, rng)
    done = np.array([False, True, False])
    payload = cpp_step_bytes(SPEC, data, done)
    assert len(payload) == SPEC.step_payload_size(ended=1)
    step = p.decode_step(SPEC, payload)
    assert step.image.dtype == np.uint8 and step.image.shape == (3, 2, I)
    np.testing.assert_array_equal(step.image, data["image"])
    np.testing.assert_array_equal(step.final_image[done], data["final_image"][done])
    assert not step.final_image[~done].any()          # carried for the ended env only; nobody reads the rest
    np.testing.assert_array_equal(step.kinematics, data["kinematics"].astype(np.float32))
    # And the learner's own encoder (the fake sims') writes the same bytes.
    mine = p.Step(decision=7, obs=data["obs"].astype(np.float32), state=data["state"].astype(np.float32),
                  mask=data["mask"], layout=data["layout"].astype(np.uint16), present=data["present"].astype(bool),
                  reward=data["reward"].astype(np.float32), done=done, terminated=done,
                  final_obs=data["obs"].astype(np.float32), final_state=data["state"].astype(np.float32),
                  episode_info=data["info"].astype(np.float32), episode_seed=np.full(3, p.NO_EPISODE_SEED, "<u4"),
                  kinematics=data["kinematics"].astype(np.float32), image=data["image"],
                  final_image=data["final_image"])
    assert p.encode_step(SPEC, mine) == payload


def test_with_device_buffers_the_images_leave_the_step_but_the_final_images_stay():
    rng = np.random.default_rng(4)
    data = fields(SPEC, rng)
    done = np.array([True, False, True])
    names = [name for name, _, _ in SPEC.step_layout(device=True)]
    assert "image" not in names and names[-1] == "final_image"
    assert len(cpp_step_bytes(SPEC, data, done, device=True)) == SPEC.step_payload_size(ended=2, device=True)


def test_a_stage_without_a_camera_is_protocol_20_on_the_wire():
    names = [name for name, _, _ in PLAIN.step_layout()]
    assert names == ["obs", "state", "mask", "layout", "present", "reward", "done", "terminated", "final_obs",
                     "final_state", "episode_info", "episode_seed", "kinematics"]
    rng = np.random.default_rng(5)
    data = fields(PLAIN, rng)
    done = np.array([False, False, True])
    payload = cpp_step_bytes(PLAIN, data, done)
    step = p.decode_step(PLAIN, payload)
    assert step.image is None and step.final_image is None
    # Protocol 20's SPEC ended at the kinematics width (84 bytes of SpecMsg); 21 adds ImageBytes = 0, 22 LookHeads = 0,
    # and nothing else.
    assert cpp_spec_bytes(PLAIN)[84:92] == struct.pack("<II", 0, 0)
    assert len(payload) == PLAIN.step_payload_size(ended=1)


# ------------------------------------------------------------------ the decode table


def test_decoding_every_byte_is_the_sims_decode_pixel_exactly():
    values = torch.arange(256, dtype=torch.uint8)
    pixels = torch.stack([values, values, values, values, values], dim=-1)   # 256 pixels, every value in every byte
    image = pixels.reshape(1, -1)                                    # one frame of 16 x 16
    decoded = decode_image(image, 16, 16).reshape(256, 5).numpy()
    expected = np.array([decode_pixel(pixel) for pixel in pixels.numpy()], dtype=np.float32)
    np.testing.assert_array_equal(decoded, expected)
    # The table: 255 -> 1.0 else b / 254; (b - 128) / 125; b / 255; b & 31; (b >> 5) & 1; the slot byte apart.
    assert decoded[255, 0] == 1.0 and decoded[254, 0] == 1.0 and decoded[127, 0] == np.float32(127) / np.float32(254)
    assert decoded[3, 1] == -1.0 and decoded[253, 1] == 1.0 and decoded[128, 1] == 0.0
    assert decoded[0x3F, 3] == 31.0 and decoded[0x3F, 4] == 1.0 and decoded[0x47, 3] == 7.0 and decoded[0x47, 4] == 0
    assert decoded[0x96, 3] == 22.0 and decoded[0x96, 4] == 0.0      # bits 6-7 (reserved) are no part of either
    np.testing.assert_array_equal(decode_slots(image, 16, 16).reshape(256).numpy(), np.arange(256))


def test_the_sims_encoding_round_trips_through_the_learners_decoding():
    hits = [  # class, distance, rise over the feet, normal z, objective, slot
        (SKY, 2000.0, 0.0, 0.0, False, 0), (1, 0.25, 0.0, 1.0, False, 0), (1, 1.0, -25.0, 0.5, True, 0),
        (2, 100.0, 25.0, 0.0, False, 0), (2, 1000.0, 40.0, 0.25, True, 0), (3, 5.0, -40.0, 0.75, False, 7),
        *[(kind, 10.0, 1.0, 0.5, flag, (kind * 3) % (SLOTS + 1)) for kind in range(1, CLASSES)
          for flag in (False, True)]]
    pixels = np.array([encode_pixel(*hit) for hit in hits], dtype=np.uint8)
    frame = np.zeros((len(hits) * 5,), np.uint8)
    frame[: pixels.size] = pixels.reshape(-1)
    decoded = decode_image(torch.from_numpy(frame)[None], 1, len(hits))[0, 0].numpy()
    slots = decode_slots(torch.from_numpy(frame)[None], 1, len(hits))[0, 0].numpy()
    for (kind, distance, rise, normal, flag, slot), (d, h, n, k, o), s in zip(hits, decoded, slots):
        assert k == kind and o == float(flag) and s == slot
        assert n == pytest.approx(normal, abs=0.5 / 255 + 1e-6)
        if kind == SKY:
            assert (d, h) == (1.0, 0.0)
            continue
        scaled = np.clip(np.log(max(distance, NEAR) / NEAR) / np.log(DISTANCE_REFERENCE / NEAR), 0, 1)
        assert d == pytest.approx(scaled, abs=0.5 / 254 + 1e-6)
        assert h == pytest.approx(np.clip(rise / 25.0, -1, 1), abs=0.5 / 125 + 1e-6)


def test_a_row_without_a_frame_is_sky_at_height_zero():
    blank = p.no_frame((2, 3, I))
    assert blank.dtype == np.uint8 and tuple(blank[1, 2, :5]) == p.NO_FRAME_PIXEL == (255, 128, 0, 0, 0)
    decoded = decode_image(torch.from_numpy(blank).reshape(6, I), H, W)
    assert bool((decoded == torch.tensor([1.0, 0.0, 0.0, 0.0, 0.0])).all())
    assert bool((decode_slots(torch.from_numpy(blank).reshape(6, I), H, W) == 0).all())


# ------------------------------------------------------------------ stage.json and the rollout buffer


def test_the_sim_and_stage_json_must_agree_about_the_camera():
    vision = [{"first": 3, "height": H, "width": W, "bytes_per_pixel": 5, "image_bytes": I}, None]
    assert vision_image_bytes(vision) == I and vision_image_bytes(None) == 0
    check_image_bytes(vision, I)
    check_image_bytes(None, 0)
    with pytest.raises(ValueError, match="stage.json's camera makes"):
        check_image_bytes(vision, I + 4)
    with pytest.raises(ValueError, match="no camera"):
        check_image_bytes(None, I)
    with pytest.raises(ValueError, match="sends no image"):
        check_image_bytes(vision, 0)


def test_the_rollout_buffer_keeps_the_images_as_bytes():
    buffer = RolloutBuffer(2, 3, 2, 4, 5, 3, image_bytes=I)
    assert buffer.image.dtype == np.uint8 and buffer.image.shape == (2, 3, 2, I)
    image = np.random.default_rng(6).integers(0, 256, (3, 2, I), dtype=np.uint8)
    buffer.add_decision(np.zeros((3, 2, 4), np.float32), np.zeros((3, 5), np.float32), np.ones((3, 2, 3), bool),
                        np.zeros((3, 2)), np.zeros((3, 2)), np.zeros((3, 2)), np.zeros((3, 2)), image=image)
    np.testing.assert_array_equal(buffer.image[0], image)
    assert buffer.sequences()["image"].dtype == np.uint8
    plain = RolloutBuffer(2, 3, 2, 4, 5, 3)
    assert "image" not in plain.sequences() and plain.image.nbytes == 0


# ------------------------------------------------------------------ over a socket


def read_exact(conn: socket.socket, size: int) -> bytes:
    data = b""
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        assert chunk, "client closed early"
        data += chunk
    return data


def test_a_sim_with_a_camera_over_the_socket(tmp_path):
    """SPEC with ImageBytes, a DEVICE offer with the fourth (image) handle, declined, and a STEP with images -- the
    bytes as the C++ writes them -- read by ForgeEnv."""
    path = str(tmp_path / "seeing.sock")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(1)
    rng = np.random.default_rng(8)
    data = fields(SPEC, rng)
    done = np.array([False, True, False])
    answers = []

    def sim():
        conn = accept(listener)
        with conn:
            _, length = p.HEADER.unpack(read_exact(conn, p.HEADER.size))
            assert p.HELLO.unpack(read_exact(conn, length))[0] == p.PROTOCOL_VERSION == 23
            spec = cpp_spec_bytes(SPEC)
            conn.sendall(p.encode_header(p.MsgType.SPEC, len(spec)) + spec)
            device = struct.pack("<II", 0, SPEC.num_envs) + bytes(range(64)) * 3 + bytes(range(64, 128))
            assert len(device) == 200 + 64
            conn.sendall(p.encode_header(p.MsgType.DEVICE, len(device)) + device)
            _, length = p.HEADER.unpack(read_exact(conn, p.HEADER.size))
            answers.append(p.DEVICE_ACK.unpack(read_exact(conn, length))[0])
            step = cpp_step_bytes(SPEC, data, done)
            conn.sendall(p.encode_header(p.MsgType.STEP, len(step)) + step)
            read_exact(conn, p.HEADER.size)

    server = sim_thread(sim)
    server.start()
    env = ForgeEnv(path, connect_timeout=5.0)
    assert env.spec.image_bytes == I
    step = env.reset()
    np.testing.assert_array_equal(step.image, data["image"])
    np.testing.assert_array_equal(step.final_image[done], data["final_image"][done])
    env.close()
    joined(server, 5)
    listener.close()
    assert answers == [0]          # declined (rollouts on the CPU): the images then come over the socket
