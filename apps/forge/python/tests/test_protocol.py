"""Protocol encoding and a full lock-step exchange against a fake sim over a real Unix socket."""

import dataclasses
import socket
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


def make_step(decision: int, rng: np.random.Generator, spec: p.Spec = SPEC, envs: int | None = None,
              env_begin: int = 0) -> p.Step:
    e, a = spec.num_envs if envs is None else envs, spec.agents_per_env
    done = rng.random(e) < 0.5
    return p.Step(
        env_begin=env_begin,
        decision=decision,
        obs=rng.random((e, a, spec.obs_dim), dtype=np.float32),
        state=rng.random((e, spec.state_dim), dtype=np.float32),
        mask=rng.random((e, a, spec.num_actions)) < 0.7,
        layout=rng.integers(0, len(spec.layouts), size=(e, a), dtype=np.uint16),
        present=rng.random((e, a)) < 0.8,
        reward=rng.random((e, a), dtype=np.float32),
        done=done,
        terminated=done & (rng.random(e) < 0.5),
        final_obs=rng.random((e, a, spec.obs_dim), dtype=np.float32),
        final_state=rng.random((e, spec.state_dim), dtype=np.float32),
        episode_info=rng.random((e, a, spec.episode_info_dim), dtype=np.float32),
        episode_seed=rng.integers(0, 2**32, size=e, dtype=np.uint32),
    )


def assert_steps_equal(left: p.Step, right: p.Step) -> None:
    assert left.decision == right.decision
    assert left.env_begin == right.env_begin
    for name, *_ in SPEC.step_layout():
        np.testing.assert_array_equal(getattr(left, name), getattr(right, name))


def test_spec_round_trip():
    assert p.decode_spec(p.encode_spec(SPEC)) == SPEC


def test_spec_matches_cpp_layout():
    # SpecMsg in Protocol.h: twelve uint32 fields (goal count and env groups among them) and a 32-byte name, packed.
    assert p.SPEC.size == 12 * 4 + 32
    assert p.HEADER.size == 8
    # StepHeader: uint64 decision, uint32 first env, uint32 env count. ActHeader: uint32 first env, uint32 count.
    assert p.STEP_HEADER.size == 16
    assert p.ACT_HEADER.size == 8


def test_mode_matches_cpp_layout():
    # ModeMsg in Protocol.h: five uint32 fields (mode, seed base, episodes, flags, first seed) and a 32-byte policy
    # name, packed.
    assert p.MODE.size == 5 * 4 + 32
    assert p.decode_mode_first_seed(p.encode_mode(True, 1000, 64, "", first_seed=64)) == 64
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
            assert p.HELLO.unpack(read_exact(conn, length)) == (p.PROTOCOL_VERSION, 0, 1)

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
                body = read_exact(conn, length)
                assert p.ACT_HEADER.unpack_from(body) == (0, SPEC.num_envs)
                received_actions.append(np.frombuffer(body, dtype="<i4", offset=p.ACT_HEADER.size))

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


def test_half_batch_step_joins_both_halves(tmp_path):
    """In half-batch the sim sends each half's STEP on its own. ForgeEnv.step answers both halves, then joins their
    next STEPs into one decision, so everything but the pipelined rollout sees the pool as before."""
    spec = dataclasses.replace(SPEC, num_envs=4, env_groups=2)
    assert spec.env_groups_ranges() == [(0, 2), (2, 2)]
    rng = np.random.default_rng(5)

    def half(decision, begin):
        return make_step(decision, rng, spec=spec, envs=2, env_begin=begin)

    path = str(tmp_path / "forge.sock")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(1)
    decisions = [(half(d, 0), half(d, 2)) for d in range(3)]
    acts = []

    def fake_sim():
        conn, _ = listener.accept()
        with conn:
            read_exact(conn, p.HEADER.size + p.HELLO.size)
            spec_payload = p.encode_spec(spec)
            conn.sendall(p.encode_header(p.MsgType.SPEC, len(spec_payload)) + spec_payload)
            # Both halves to start; then, as the sim does, a half's next STEP after its maps tick.
            for part in decisions[0]:
                payload = p.encode_step(spec, part)
                conn.sendall(p.encode_header(p.MsgType.STEP, len(payload)) + payload)
            for decision in decisions[1:]:
                for part in decision:
                    msg_type, length = p.HEADER.unpack(read_exact(conn, p.HEADER.size))
                    assert msg_type == p.MsgType.ACT
                    body = read_exact(conn, length)
                    acts.append(p.ACT_HEADER.unpack_from(body))
                    payload = p.encode_step(spec, part)
                    conn.sendall(p.encode_header(p.MsgType.STEP, len(payload)) + payload)
            read_exact(conn, p.HEADER.size)

    server = threading.Thread(target=fake_sim)
    server.start()
    env = ForgeEnv(path, connect_timeout=5)
    assert env.spec == spec
    assert_steps_equal(env.reset(), p.join_steps(list(decisions[0])))
    for decision in decisions[1:]:
        actions = np.zeros((spec.num_envs, spec.agents_per_env), dtype=np.int64)
        assert_steps_equal(env.step(actions), p.join_steps(list(decision)))
    env.close()
    server.join(timeout=5)
    listener.close()
    assert acts == [(0, 2), (2, 2)] * 2


def test_a_sim_on_another_machine_is_reached_over_tcp():
    """A cluster worker's sim is "tcp://host:port": the same protocol over TCP."""
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]
    first = make_step(0, np.random.default_rng(3))

    def fake_sim():
        conn, _ = listener.accept()
        with conn:
            read_exact(conn, p.HEADER.size + p.HELLO.size)
            spec_payload = p.encode_spec(SPEC)
            conn.sendall(p.encode_header(p.MsgType.SPEC, len(spec_payload)) + spec_payload)
            payload = p.encode_step(SPEC, first)
            conn.sendall(p.encode_header(p.MsgType.STEP, len(payload)) + payload)
            read_exact(conn, p.HEADER.size)

    server = threading.Thread(target=fake_sim)
    server.start()
    env = ForgeEnv(f"tcp://127.0.0.1:{port}", connect_timeout=5)
    assert env.spec == SPEC
    assert_steps_equal(env.reset(), first)
    env.close()
    server.join(timeout=5)
    listener.close()
