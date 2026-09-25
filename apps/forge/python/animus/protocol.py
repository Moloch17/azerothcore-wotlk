"""Lock-step wire protocol, mirrored field for field from src/Bridge/Protocol.h.

Change both files together and bump PROTOCOL_VERSION.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from enum import IntEnum

import numpy as np

PROTOCOL_VERSION = 10
# Slots per class in the WEIGHTS vector (Curriculum::MAX_SPECS, the druid's four builds). A class with fewer
# builds still has the slots; they are never drawn and stay at the even 1.0.
MAX_SPECS = 4
SCENARIO_NAME_SIZE = 32
POLICY_NAME_SIZE = 32
LAYOUT_NAME_SIZE = 48
NO_EPISODE_SEED = 0xFFFFFFFF


class MsgType(IntEnum):
    HELLO = 1
    SPEC = 2
    STEP = 3
    ACT = 4
    CLOSE = 5
    MODE = 6
    WEIGHTS = 7
    REPLAY = 8


HEADER = struct.Struct("<II")  # type, payload length
HELLO = struct.Struct("<I")  # version
SPEC = struct.Struct(f"<11I{SCENARIO_NAME_SIZE}s")
LAYOUT_COUNT = struct.Struct("<I")
LAYOUT = struct.Struct(f"<II{LAYOUT_NAME_SIZE}s")  # obs dim, actions, name
STEP_HEADER = struct.Struct("<Q")  # decision counter
MODE = struct.Struct(f"<IIII{POLICY_NAME_SIZE}s")  # mode, seed base, episodes, flags, baseline policy
MODE_FLAG_SCRIPTED_OPPONENTS = 1  # the baseline plays only the opponent seats; the learner the rest
WEIGHTS_COUNT = struct.Struct("<I")  # then that many float32 weights, one per layout in SPEC order
REPLAY = struct.Struct("<IfI")  # seed base, share of training resets, count; then that many uint32 seed indexes
MAX_REPLAY_SEEDS = 65536


@dataclass(frozen=True)
class Layout:
    """One agent layout: the observation features and actions of one kind of agent (e.g. a class).

    An agent of this layout fills only obs[:obs_dim] and mask[:num_actions] of the padded arrays.
    """

    name: str
    obs_dim: int
    num_actions: int


@dataclass(frozen=True)
class Spec:
    version: int
    num_envs: int
    agents_per_env: int
    obs_dim: int  # the largest layout's; observations are padded to it
    state_dim: int
    num_actions: int  # the largest layout's; masks are padded to it
    episode_info_dim: int  # per agent
    goal_count: int  # goals a policy may pursue and send with its actions (SeatGoal); 0 = the scenario has none
    tick_ms: int
    decision_ticks: int
    episode_seconds: int
    scenario: str
    layouts: tuple[Layout, ...] = ()
    episode_info_names: tuple[str, ...] = field(default_factory=tuple)

    @property
    def decision_ms(self) -> int:
        return self.tick_ms * self.decision_ticks

    def step_layout(self) -> list[tuple[str, np.dtype, tuple[int, ...]]]:
        """STEP payload arrays after the header, in wire order: (name, dtype, shape)."""
        e, a = self.num_envs, self.agents_per_env
        f32, u8, u16, u32 = np.dtype("<f4"), np.dtype("u1"), np.dtype("<u2"), np.dtype("<u4")
        return [
            ("obs", f32, (e, a, self.obs_dim)),
            ("state", f32, (e, self.state_dim)),
            ("mask", u8, (e, a, self.num_actions)),
            ("layout", u16, (e, a)),
            ("present", u8, (e, a)),
            ("reward", f32, (e, a)),
            ("done", u8, (e,)),
            ("terminated", u8, (e,)),
            ("final_obs", f32, (e, a, self.obs_dim)),
            ("final_state", f32, (e, self.state_dim)),
            ("episode_info", f32, (e, a, self.episode_info_dim)),
            ("episode_seed", u32, (e,)),
        ]

    def step_payload_size(self) -> int:
        size = STEP_HEADER.size
        for _, dtype, shape in self.step_layout():
            size += dtype.itemsize * int(np.prod(shape))
        return size


@dataclass
class Step:
    decision: int
    obs: np.ndarray  # [E, A, O] float32, each agent padded to O
    state: np.ndarray  # [E, S] float32
    mask: np.ndarray  # [E, A, N] bool, each agent padded to N
    layout: np.ndarray  # [E, A] uint16, index into Spec.layouts
    present: np.ndarray  # [E, A] bool, False for a seat without a character this episode (not a sample)
    reward: np.ndarray  # [E, A] float32
    done: np.ndarray  # [E] bool
    terminated: np.ndarray  # [E] bool
    final_obs: np.ndarray  # [E, A, O] float32, valid where done
    final_state: np.ndarray  # [E, S] float32, valid where done
    episode_info: np.ndarray  # [E, A, K] float32, valid where done
    episode_seed: np.ndarray  # [E] uint32, evaluation seed index where done; NO_EPISODE_SEED for training


def encode_spec(spec: Spec) -> bytes:
    body = SPEC.pack(
        spec.version,
        spec.num_envs,
        spec.agents_per_env,
        spec.obs_dim,
        spec.state_dim,
        spec.num_actions,
        spec.episode_info_dim,
        spec.goal_count,
        spec.tick_ms,
        spec.decision_ticks,
        spec.episode_seconds,
        spec.scenario.encode("ascii"),
    )
    body += LAYOUT_COUNT.pack(len(spec.layouts))
    for layout in spec.layouts:
        body += LAYOUT.pack(layout.obs_dim, layout.num_actions, layout.name.encode("ascii"))
    return body + ",".join(spec.episode_info_names).encode("ascii")


def decode_spec(payload: bytes) -> Spec:
    fields = SPEC.unpack_from(payload)
    offset = SPEC.size
    (count,) = LAYOUT_COUNT.unpack_from(payload, offset)
    offset += LAYOUT_COUNT.size
    layouts = []
    for _ in range(count):
        obs_dim, num_actions, name = LAYOUT.unpack_from(payload, offset)
        offset += LAYOUT.size
        layouts.append(Layout(name.split(b"\0", 1)[0].decode("ascii"), obs_dim, num_actions))
    names = payload[offset:].decode("ascii")
    return Spec(
        *fields[:11],
        scenario=fields[11].split(b"\0", 1)[0].decode("ascii"),
        layouts=tuple(layouts),
        episode_info_names=tuple(names.split(",")) if names else (),
    )


def encode_step(spec: Spec, step: Step) -> bytes:
    parts = [STEP_HEADER.pack(step.decision)]
    for name, dtype, shape in spec.step_layout():
        array = np.ascontiguousarray(getattr(step, name), dtype=dtype).reshape(shape)
        parts.append(array.tobytes())
    return b"".join(parts)


def decode_step(spec: Spec, payload: bytes | bytearray | memoryview) -> Step:
    """Decode a STEP payload. Arrays are copies, so the receive buffer can be reused."""
    (decision,) = STEP_HEADER.unpack_from(payload)
    offset = STEP_HEADER.size
    arrays = {}
    for name, dtype, shape in spec.step_layout():
        count = int(np.prod(shape))
        array = np.frombuffer(payload, dtype=dtype, count=count, offset=offset).reshape(shape).copy()
        offset += dtype.itemsize * count
        if dtype == np.dtype("u1"):
            array = array.astype(bool)
        arrays[name] = array
    return Step(decision=decision, **arrays)


def encode_mode(evaluate: bool, seed_base: int = 0, episodes: int = 0, baseline: str = "",
                opponents_only: bool = False) -> bytes:
    name = baseline.encode("ascii")
    if len(name) >= POLICY_NAME_SIZE:
        raise ValueError(f"baseline policy name '{baseline}' is too long")
    flags = MODE_FLAG_SCRIPTED_OPPONENTS if opponents_only else 0
    return MODE.pack(int(evaluate), seed_base, episodes, flags, name)


def decode_mode(payload: bytes) -> tuple[bool, int, int, str, bool]:
    mode, seed_base, episodes, flags, name = MODE.unpack(payload)
    return (bool(mode), seed_base, episodes, name.split(b"\0", 1)[0].decode("ascii"),
            bool(flags & MODE_FLAG_SCRIPTED_OPPONENTS))


def encode_weights(weights) -> bytes:
    """WEIGHTS payload: how often training episodes draw each layout, in the SPEC's layout order."""
    array = np.asarray(weights, dtype="<f4")
    if array.ndim != 1:
        raise ValueError("layout weights must be a flat sequence, one per layout")
    return WEIGHTS_COUNT.pack(len(array)) + array.tobytes()


def decode_weights(payload: bytes | bytearray | memoryview) -> np.ndarray:
    (count,) = WEIGHTS_COUNT.unpack_from(payload)
    return np.frombuffer(payload, dtype="<f4", count=count, offset=WEIGHTS_COUNT.size).copy()


def encode_replay(seed_base: int, fraction: float, seeds) -> bytes:
    """REPLAY payload: evaluation seeds (of `seed_base`) that training resets rebuild, `fraction` of the time."""
    array = np.asarray(sorted(set(int(s) for s in seeds)), dtype="<u4")
    if len(array) > MAX_REPLAY_SEEDS:
        raise ValueError(f"at most {MAX_REPLAY_SEEDS} replay seeds, got {len(array)}")
    return REPLAY.pack(seed_base, float(fraction), len(array)) + array.tobytes()


def decode_replay(payload: bytes | bytearray | memoryview) -> tuple[int, float, np.ndarray]:
    seed_base, fraction, count = REPLAY.unpack_from(payload)
    return seed_base, fraction, np.frombuffer(payload, dtype="<u4", count=count, offset=REPLAY.size).copy()


def encode_header(msg_type: MsgType, length: int) -> bytes:
    return HEADER.pack(int(msg_type), length)
