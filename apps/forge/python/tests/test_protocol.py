"""Protocol encoding and a full lock-step exchange against a fake sim over a real Unix socket."""

import socket
import struct
import threading

import numpy as np
import pytest

from animus import protocol as p
from animus.env import ForgeEnv

SPEC = p.Spec(
    version=p.PROTOCOL_VERSION,
    num_envs=3,
    agents_per_env=2,
    obs_dim=4,
    state_dim=5,
    num_actions=3,
    episode_info_dim=2,
    goal_count=0,
    tick_ms=50,
    decision_ticks=1,
    episode_seconds=60,
    scenario="fake",
    layouts=(p.Layout("warrior_dps", 4, 3), p.Layout("priest_heal", 2, 2)),
    episode_info_names=("damage", "dps"),
)


def make_step(decision: int, rng: np.random.Generator) -> p.Step:
    e, a = SPEC.num_envs, SPEC.agents_per_env
    done = rng.random(e) < 0.5
    return p.Step(
        decision=decision,
        obs=rng.random((e, a, SPEC.obs_dim), dtype=np.float32),
        state=rng.random((e, SPEC.state_dim), dtype=np.float32),
        mask=rng.random((e, a, SPEC.num_actions)) < 0.7,
        layout=rng.integers(0, len(SPEC.layouts), size=(e, a), dtype=np.uint16),
        present=rng.random((e, a)) < 0.8,
        reward=rng.random((e, a), dtype=np.float32),
        done=done,
        terminated=done & (rng.random(e) < 0.5),
        final_obs=rng.random((e, a, SPEC.obs_dim), dtype=np.float32),
        final_state=rng.random((e, SPEC.state_dim), dtype=np.float32),
        episode_info=rng.random((e, a, SPEC.episode_info_dim), dtype=np.float32),
        episode_seed=rng.integers(0, 2**32, size=e, dtype=np.uint32),
    )


def assert_steps_equal(left: p.Step, right: p.Step) -> None:
    assert left.decision == right.decision
    for name, *_ in SPEC.step_layout():
        np.testing.assert_array_equal(getattr(left, name), getattr(right, name))


def test_spec_round_trip():
    assert p.decode_spec(p.encode_spec(SPEC)) == SPEC


def test_spec_matches_cpp_layout():
    # SpecMsg in Protocol.h: eleven uint32 fields (goal count among them) and a 32-byte name, packed.
    assert p.SPEC.size == 11 * 4 + 32
    assert p.HEADER.size == 8


def test_mode_matches_cpp_layout():
    # ModeMsg in Protocol.h: four uint32 fields (mode, seed base, episodes, flags) and a 32-byte policy name, packed.
    assert p.MODE.size == 4 * 4 + 32
    assert p.decode_mode(p.encode_mode(True, 1000, 128, "fight")) == (True, 1000, 128, "fight", False)
    assert p.decode_mode(p.encode_mode(True, 1000, 128, "fight", opponents_only=True)) == (
        True, 1000, 128, "fight", True)
    assert p.MODE.unpack(p.encode_mode(True, 1, 2, "fight", opponents_only=True))[3] == p.MODE_FLAG_SCRIPTED_OPPONENTS


def test_weights_round_trip():
    # WeightsHeader in Protocol.h: one uint32 count, then that many float32 weights.
    assert p.WEIGHTS_COUNT.size == 4
    payload = p.encode_weights([1.0, 2.5, 0.0])
    assert len(payload) == 4 + 3 * 4
    assert list(p.decode_weights(payload)) == [1.0, 2.5, 0.0]
    assert list(p.decode_weights(p.encode_weights([]))) == []


def test_step_round_trip_and_size():
    step = make_step(7, np.random.default_rng(0))
    payload = p.encode_step(SPEC, step)
    assert len(payload) == SPEC.step_payload_size()
    assert_steps_equal(p.decode_step(SPEC, payload), step)


def read_exact(conn: socket.socket, size: int) -> bytes:
    data = b""
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        assert chunk, "client closed early"
        data += chunk
    return data


def test_lockstep_exchange(tmp_path):
    path = str(tmp_path / "forge.sock")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(1)

    rng = np.random.default_rng(1)
    steps = [make_step(i, rng) for i in range(3)]
    received_actions = []
    closed = threading.Event()

    def fake_sim():
        conn, _ = listener.accept()
        with conn:
            msg_type, length = p.HEADER.unpack(read_exact(conn, p.HEADER.size))
            assert msg_type == p.MsgType.HELLO
            assert struct.unpack("<I", read_exact(conn, length))[0] == p.PROTOCOL_VERSION

            spec_payload = p.encode_spec(SPEC)
            conn.sendall(p.encode_header(p.MsgType.SPEC, len(spec_payload)) + spec_payload)

            for step in steps:
                payload = p.encode_step(SPEC, step)
                conn.sendall(p.encode_header(p.MsgType.STEP, len(payload)) + payload)

                msg_type, length = p.HEADER.unpack(read_exact(conn, p.HEADER.size))
                if msg_type == p.MsgType.CLOSE:
                    closed.set()
                    return
                assert msg_type == p.MsgType.ACT
                received_actions.append(np.frombuffer(read_exact(conn, length), dtype="<i4"))

    server = threading.Thread(target=fake_sim)
    server.start()

    env = ForgeEnv(path, connect_timeout=5)
    assert env.spec == SPEC

    assert_steps_equal(env.reset(), steps[0])
    sent = []
    for expected in steps[1:]:
        actions = rng.integers(0, SPEC.num_actions, size=(SPEC.num_envs, SPEC.agents_per_env))
        sent.append(actions)
        assert_steps_equal(env.step(actions), expected)
    env.close()

    server.join(timeout=5)
    listener.close()

    assert closed.is_set()
    assert len(received_actions) == len(sent)
    for got, want in zip(received_actions, sent):
        np.testing.assert_array_equal(got.reshape(want.shape), want)


def test_replay_round_trip():
    # ReplayHeader in Protocol.h: uint32 seed base, float32 fraction, uint32 count, then count uint32 seed indexes.
    assert p.REPLAY.size == 12
    payload = p.encode_replay(1000, 0.2, [7, 3, 7, 12])
    assert len(payload) == 12 + 3 * 4  # sorted and deduplicated
    seed_base, fraction, seeds = p.decode_replay(payload)
    assert seed_base == 1000 and fraction == pytest.approx(0.2) and list(seeds) == [3, 7, 12]
    assert list(p.decode_replay(p.encode_replay(1000, 0.0, []))[2]) == []
