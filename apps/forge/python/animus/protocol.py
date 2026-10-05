"""Lock-step wire protocol, mirrored field for field from src/Bridge/Protocol.h.

Change both files together and bump PROTOCOL_VERSION.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from enum import IntEnum

import numpy as np

PROTOCOL_VERSION = 20
# 20: SPEC announces a kinematics width after the scenario name, and every STEP ends with one kinematic sample per
# agent (FORMAT.md section 3, animus.human.motion): the body the style reward and the realism score read.
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
    DEVICE = 9
    DEVICE_ACK = 10
    PROGRESS = 11       # f32 progress through the stage's budget, f32 shaping scale (18), f32 cost scale (19)
    EXPLORE_STARTS = 12  # the cells a wing's training runs start from (Go-Explore, animus.explore; 18)


HEADER = struct.Struct("<II")  # type, payload length
HELLO = struct.Struct("<III")  # version, this learner's rank, data-parallel learners (0 and 1 alone)
SPEC = struct.Struct(f"<12I{SCENARIO_NAME_SIZE}sI")  # ..., scenario name, kinematics width (20)
LAYOUT_COUNT = struct.Struct("<I")
LAYOUT = struct.Struct(f"<II{LAYOUT_NAME_SIZE}s")  # obs dim, actions, name
STEP_HEADER = struct.Struct("<QII")  # decision counter, first env, env count
ACT_HEADER = struct.Struct("<II")  # first env, env count; then that many envs' actions (and goals)
# mode, seed base, episodes, flags, first seed, held-out arena (index + 1, 0 = the stage's own; 18), baseline policy
MODE = struct.Struct(f"<IIIIII{POLICY_NAME_SIZE}s")
MODE_FLAG_SCRIPTED_OPPONENTS = 1  # the baseline plays only the opponent seats; the learner the rest
WEIGHTS_COUNT = struct.Struct("<I")  # then that many float32 weights, one per layout in SPEC order
REPLAY = struct.Struct("<IfI")  # seed base, share of training resets, count; then that many uint32 seed indexes
MAX_REPLAY_SEEDS = 65536
EXPLORE_STARTS = struct.Struct("<fI")  # share of a wing's training resets, count; then that many EXPLORE_CELL
EXPLORE_CELL = struct.Struct("<II4IIf")  # arena, tier, cleared packs (4 words of 24 bits), yard / 16, weight
MAX_EXPLORE_STARTS = 64
# DEVICE (protocol 15): the sim's device buffers for this learner's obs, state and mask -- GPU, envs, then the three
# hipIpcMemHandle_t -- offered after SPEC; DEVICE_ACK answers 1 when they were opened (animus.device).
DEVICE = struct.Struct("<II64s64s64s")
DEVICE_ACK = struct.Struct("<I")
# What a STEP leaves out when the learner reads them from the device buffers.
DEVICE_FIELDS = ("obs", "state", "mask")


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
    # 2 = half-batch: the sim ticks the two halves' maps in turn, and each STEP and ACT covers one half (env_groups()).
    # A learner may still answer both halves together (ForgeEnv.step); answering each as it comes (the rollout's
    # pipelined loop) is what lets its inference run while the other half's maps tick.
    env_groups: int = 1
    # Floats per agent of each STEP's kinematics (protocol 20): motion.SAMPLE_DIM from a sim that sends them, 0 none.
    kinematics_dim: int = 0

    @property
    def decision_ms(self) -> int:
        return self.tick_ms * self.decision_ticks

    def env_groups_ranges(self) -> list[tuple[int, int]]:
        """(first env, env count) of each group, in the order the sim sends them: contiguous halves of the envs."""
        if self.env_groups <= 1:
            return [(0, self.num_envs)]
        half = self.num_envs // 2
        return [(0, half), (half, self.num_envs - half)]

    def step_layout(self, envs: int | None = None, ended: int | None = None,
                    device: bool = False) -> list[tuple[str, np.dtype, tuple[int, ...]]]:
        """STEP payload arrays after the header, in wire order: (name, dtype, shape), for `envs` envs (all of them by
        default; a half-batch STEP carries one group's). final_obs, final_state and (protocol 18) episode_info carry
        only the `ended` envs whose done is set, in env order (every env's by default: the largest a STEP can be) -- protocol 14; the others'
        would be ~half of every STEP for rows nobody reads. With `device` (protocol 15) obs, state and mask are in the
        sim's device buffers instead, and not in the STEP."""
        e, a = self.num_envs if envs is None else envs, self.agents_per_env
        d = e if ended is None else ended
        f32, u8, u16, u32 = np.dtype("<f4"), np.dtype("u1"), np.dtype("<u2"), np.dtype("<u4")
        layout = [
            ("obs", f32, (e, a, self.obs_dim)),
            ("state", f32, (e, self.state_dim)),
            ("mask", u8, (e, a, self.num_actions)),
            ("layout", u16, (e, a)),
            ("present", u8, (e, a)),
            ("reward", f32, (e, a)),
            ("done", u8, (e,)),
            ("terminated", u8, (e,)),
            ("final_obs", f32, (d, a, self.obs_dim)),
            ("final_state", f32, (d, self.state_dim)),
            ("episode_info", f32, (d, a, self.episode_info_dim)),
            ("episode_seed", u32, (e,)),
            # Each agent's body after the transition (protocol 20): [t, x, y, z, yaw, pitch, mode, mounted, speed,
            # in_combat], the new episode's first sample where done is set; zeros for an agent without a body.
            ("kinematics", f32, (e, a, self.kinematics_dim)),
        ]
        return [item for item in layout if item[0] not in DEVICE_FIELDS] if device else layout

    def step_payload_size(self, envs: int | None = None, ended: int | None = None, device: bool = False) -> int:
        size = STEP_HEADER.size
        for _, dtype, shape in self.step_layout(envs, ended, device):
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
    env_begin: int = 0  # the first env these rows are: a half-batch STEP covers envs [env_begin, env_begin + E)
    # [E, A, K] float32 kinematic samples (protocol 20, Spec.kinematics_dim); None where nobody made any (a step built
    # by hand), which encodes as zeros.
    kinematics: np.ndarray | None = None


def rows_of(step: Step, begin: int, count: int) -> Step:
    """Envs [begin, begin + count) of a decision, as the STEP of that group would carry them."""
    if begin == 0 and count == step.done.shape[0]:
        return step
    names = [name for name in Step.__dataclass_fields__ if name not in ("decision", "env_begin")]
    return Step(decision=step.decision, env_begin=step.env_begin + begin,
                **{name: None if getattr(step, name) is None else getattr(step, name)[begin:begin + count]
                   for name in names})


def join_steps(parts: list[Step]) -> Step:
    """One decision from its groups' STEPs, in env order."""
    if len(parts) == 1:
        return parts[0]
    names = [name for name in Step.__dataclass_fields__ if name not in ("decision", "env_begin")]
    return Step(decision=parts[0].decision, env_begin=0,
                **{name: _join_field([getattr(part, name) for part in parts], parts) for name in names})


def _join_field(arrays, parts: list[Step]):
    """One field of every group, joined; an optional field (kinematics) some groups lack is zeros in their rows."""
    if all(array is None for array in arrays):
        return None
    if any(array is None for array in arrays):
        like = next(array for array in arrays if array is not None)
        arrays = [np.zeros((part.done.shape[0], *like.shape[1:]), like.dtype) if array is None else array
                  for array, part in zip(arrays, parts)]
    return _concatenate(arrays)


def _concatenate(arrays):
    """numpy's concatenate, or torch's for the device views of protocol 15. Groups of one decision are consecutive
    rows of one buffer, so their views are joined without a copy when they are."""
    if hasattr(arrays[0], "detach"):
        import torch
        return torch.cat(arrays)
    return np.concatenate(arrays)


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
        spec.env_groups,
        spec.scenario.encode("ascii"),
        spec.kinematics_dim,
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
        env_groups=fields[11],
        scenario=fields[12].split(b"\0", 1)[0].decode("ascii"),
        kinematics_dim=fields[13],
        layouts=tuple(layouts),
        episode_info_names=tuple(names.split(",")) if names else (),
    )


# Carried for the ended envs only (Spec.step_layout); the decoder gives them back full-sized, zero elsewhere. Episode
# info from protocol 18: every reader looks only where done is set.
ENDED_ONLY = ("final_obs", "final_state", "episode_info")


def encode_step(spec: Spec, step: Step) -> bytes:
    envs = step.done.shape[0]
    done = np.asarray(step.done, dtype=bool)
    parts = [STEP_HEADER.pack(step.decision, step.env_begin, envs)]
    for name, dtype, shape in spec.step_layout(envs, int(done.sum())):
        array = getattr(step, name)
        if array is None:
            array = np.zeros(shape, dtype)
        if name in ENDED_ONLY:
            array = np.asarray(array)[done]
        parts.append(np.ascontiguousarray(array, dtype=dtype).reshape(shape).tobytes())
    return b"".join(parts)


_DECODE_LAYOUTS: dict[tuple, list] = {}


def _decode_layout(spec: Spec, envs: int, device: bool) -> list[tuple[str, np.dtype, tuple[int, ...], bool]]:
    """Spec.step_layout for a STEP of `envs` envs, with each field's u1 flag, built once per shape: a STEP is decoded
    every decision, and the layout of a given spec and env count never changes."""
    key = (spec, envs, device)
    layout = _DECODE_LAYOUTS.get(key)
    if layout is None:
        layout = _DECODE_LAYOUTS[key] = [(name, dtype, shape, dtype == np.dtype("u1"))
                                         for name, dtype, shape in spec.step_layout(envs, device=device)]
    return layout


def decode_step(spec: Spec, payload: bytes | bytearray | memoryview, device=None) -> Step:
    """Decode a STEP payload. Arrays are copies, so the receive buffer can be reused. Raises ValueError when the
    payload is not the size its envs and ended envs make. With `device` (animus.device.DeviceBuffers, protocol 15)
    obs, state and mask are views of the sim's device buffers, valid until this group's next STEP."""
    decision, env_begin, envs = STEP_HEADER.unpack_from(payload)
    offset = STEP_HEADER.size
    arrays = {}
    if device is not None:
        arrays["obs"], arrays["state"], arrays["mask"] = device.rows(env_begin, envs)
    done = ended = None
    for name, dtype, shape, flag in _decode_layout(spec, envs, device is not None):
        if name in ENDED_ONLY:
            if ended is None:
                ended = np.flatnonzero(done)
            rows = (len(ended),) + shape[1:]
            count = int(np.prod(rows))
            array = np.zeros(shape, dtype)
            array[ended] = np.frombuffer(payload, dtype=dtype, count=count, offset=offset).reshape(rows)
        else:
            count = int(np.prod(shape))
            # A u1 flag is read as bool directly (the sim writes 0 or 1): one copy, not a copy and a conversion.
            array = np.frombuffer(payload, dtype=bool if flag else dtype, count=count, offset=offset).reshape(
                shape).copy()
        offset += dtype.itemsize * count
        if name == "done":
            done = array
        arrays[name] = array
    if offset != len(payload):
        raise ValueError(f"STEP of {len(payload)} bytes does not hold the {envs} envs it says")
    return Step(decision=decision, env_begin=env_begin, **arrays)


def encode_act(env_begin: int, actions: np.ndarray, goals: np.ndarray | None = None) -> bytes:
    """ACT payload for envs [env_begin, env_begin + len(actions)): [E, A] actions, then the goals when the policy has a
    goal head: [E, A, 2], primary then secondary (-1 none; MappoTrainer.wire_goals) -- protocol 17."""
    actions = np.ascontiguousarray(actions, dtype="<i4")
    payload = ACT_HEADER.pack(env_begin, actions.shape[0]) + actions.tobytes()
    if goals is not None:
        payload += np.ascontiguousarray(goals, dtype="<i4").tobytes()
    return payload


def encode_mode(evaluate: bool, seed_base: int = 0, episodes: int = 0, baseline: str = "",
                opponents_only: bool = False, first_seed: int = 0, arena: int = 0) -> bytes:
    """MODE payload. An evaluation plays seed indexes [first_seed, first_seed + episodes) of seed_base, so a
    cluster's sims can each play their own share of one evaluation's seeds (ClusterEnv). `arena` pins it to a held-out
    arena (stage.json's index + 1; 0 = the stage's own draw, protocol 18)."""
    name = baseline.encode("ascii")
    if len(name) >= POLICY_NAME_SIZE:
        raise ValueError(f"baseline policy name '{baseline}' is too long")
    flags = MODE_FLAG_SCRIPTED_OPPONENTS if opponents_only else 0
    return MODE.pack(int(evaluate), seed_base, episodes, flags, first_seed, arena, name)


def decode_mode(payload: bytes) -> tuple[bool, int, int, str, bool]:
    mode, seed_base, episodes, flags, _first_seed, _arena, name = MODE.unpack(payload)
    return (bool(mode), seed_base, episodes, name.split(b"\0", 1)[0].decode("ascii"),
            bool(flags & MODE_FLAG_SCRIPTED_OPPONENTS))


def decode_mode_arena(payload: bytes) -> int:
    """The held-out arena a MODE pins (index + 1; 0 = none)."""
    return MODE.unpack(payload)[5]


def decode_mode_first_seed(payload: bytes) -> int:
    """The first seed index a MODE's evaluation plays (0 unless a cluster split the seeds)."""
    return MODE.unpack(payload)[4]


def encode_progress(progress: float, shaping_scale: float = 1.0, cost_scale: float = 1.0) -> bytes:
    """PROGRESS payload: how far through its budget the stage's training is, 0 to 1 (protocol 17), what every
    Shaping reward term is paid times, 0 to 1 (the fade ladder, protocol 18), and what every noise price is paid
    times, 0 to 1 (the cost ladder, protocol 19; ProgressMsg)."""
    def unit(value: float) -> float:
        return float(min(1.0, max(0.0, value)))
    return struct.pack("<fff", unit(progress), unit(shaping_scale), unit(cost_scale))


def encode_weights(weights) -> bytes:
    """WEIGHTS payload: how often training episodes draw each layout, in the SPEC's layout order."""
    array = np.asarray(weights, dtype="<f4")
    if array.ndim != 1:
        raise ValueError("layout weights must be a flat sequence, one per layout")
    return WEIGHTS_COUNT.pack(len(array)) + array.tobytes()


def decode_weights(payload: bytes | bytearray | memoryview) -> np.ndarray:
    (count,) = WEIGHTS_COUNT.unpack_from(payload)
    return np.frombuffer(payload, dtype="<f4", count=count, offset=WEIGHTS_COUNT.size).copy()


def encode_explore_starts(share: float, cells) -> bytes:
    """EXPLORE_STARTS payload: `cells` -- (arena, tier, packs (4 words), yard, weight) -- that `share` of a wing's
    training resets start from instead of the door."""
    cells = list(cells)
    if len(cells) > MAX_EXPLORE_STARTS:
        raise ValueError(f"at most {MAX_EXPLORE_STARTS} explore starts, got {len(cells)}")
    return EXPLORE_STARTS.pack(float(share), len(cells)) + b"".join(
        EXPLORE_CELL.pack(arena, tier, *packs, yard, float(weight)) for arena, tier, packs, yard, weight in cells)


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
