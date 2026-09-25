"""Vectorised environment client for the Animus Forge sim.

The sim is the server and runs lock-step: it sends one STEP with every env's observations and
blocks until it gets one ACT back. All envs auto-reset inside the sim.
"""

from __future__ import annotations

import dataclasses
import socket
import time

import numpy as np

from . import protocol as p


class ForgeEnv:
    def __init__(self, socket_path: str, connect_timeout: float = 600.0, rank: int = 0, ranks: int = 1):
        """`rank` of `ranks` data-parallel learners sharing the sim's pool (animus.parallel): the sim hands each its
        own share of the envs, and this learner sees only its share."""
        self.socket_path = socket_path
        self.sock = self._connect(socket_path, connect_timeout)
        hello = p.HELLO.pack(p.PROTOCOL_VERSION, rank, ranks)
        self.sock.sendall(p.encode_header(p.MsgType.HELLO, len(hello)) + hello)

        msg_type, payload = self._receive()
        if msg_type != p.MsgType.SPEC:
            raise ConnectionError(f"expected SPEC, got message type {msg_type}")

        self.spec = p.decode_spec(payload)
        if self.spec.version != p.PROTOCOL_VERSION:
            raise ConnectionError(f"sim speaks protocol {self.spec.version}, client {p.PROTOCOL_VERSION}")

        # A group's STEP (the whole pool, or one half in half-batch) is the largest message the sim sends.
        self.groups = self.spec.env_groups_ranges()
        self._step_buffer = bytearray(max(self.spec.step_payload_size(count) for _, count in self.groups))
        self._pending: p.Step | None = None

    @staticmethod
    def _connect(path: str, timeout: float) -> socket.socket:
        """Retry until the sim is listening: the server may still be loading the world. `path` is a Unix socket, or
        "tcp://host:port" for a sim on another machine (a cluster worker)."""
        deadline = time.monotonic() + timeout
        while True:
            if path.startswith("tcp://"):
                host, _, port = path[len("tcp://"):].rpartition(":")
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                target = (host, int(port))
            else:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                target = path
            try:
                sock.connect(target)
                return sock
            except (FileNotFoundError, ConnectionRefusedError, OSError):
                sock.close()
                if time.monotonic() > deadline:
                    raise
                time.sleep(1.0)

    def reset(self) -> p.Step:
        """The first STEP after connecting: freshly reset envs. Its reward/done are meaningless."""
        if self._pending is None:
            self._pending = self._receive_decision()
        return self._pending

    def step(self, actions: np.ndarray, goals: np.ndarray | None = None) -> p.Step:
        """Send [E, A] actions, with the goals each agent is pursuing when the policy has a goal head, and return the
        next STEP. Goals are what the sim scores, reports and shows a party's teammates; they mask nothing."""
        expected = (self.spec.num_envs, self.spec.agents_per_env)
        if np.shape(actions) != expected:
            raise ValueError(f"actions must have shape {expected}, got {np.shape(actions)}")
        if goals is not None and np.shape(goals) != expected:
            raise ValueError(f"goals must have shape {expected}, got {np.shape(goals)}")

        # In half-batch both halves are answered before either STEP is read: the sim has what it needs for both
        # ticks, so this works exactly as a whole-pool step, only without the overlap the pipelined rollout gets.
        for begin, count in self.groups:
            rows = slice(begin, begin + count)
            self.send_act(begin, actions[rows], goals[rows] if goals is not None else None)
        self._pending = self._receive_decision()
        return self._pending

    def send_act(self, env_begin: int, actions: np.ndarray, goals: np.ndarray | None = None) -> None:
        """Answer one group's STEP: [count, A] actions (and goals) for envs [env_begin, env_begin + count)."""
        payload = p.encode_act(env_begin, actions, goals)
        self.sock.sendall(p.encode_header(p.MsgType.ACT, len(payload)) + payload)

    def receive_step(self) -> p.Step:
        """The next group's STEP as it comes (one half in half-batch, env_begin says which)."""
        return self._receive_step()

    def set_mode(self, evaluate: bool, seed_base: int = 0, episodes: int = 0, baseline: str = "",
                 opponents_only: bool = False, first_seed: int = 0) -> p.Step:
        """Switch the sim between training and seeded evaluation (see protocol MODE). With `opponents_only` the
        baseline plays only the opponent seats of self-play episodes and the actions sent play the rest.

        Every env resets; the returned STEP holds the fresh observations and, like the first one, no transition.
        """
        payload = p.encode_mode(evaluate, seed_base, episodes, baseline, opponents_only, first_seed)
        self.sock.sendall(p.encode_header(p.MsgType.MODE, len(payload)) + payload)
        self._pending = self._receive_decision()
        return self._pending

    def set_layout_weights(self, weights) -> None:
        """How often training episodes draw each layout, in the SPEC's layout order (see protocol WEIGHTS).

        The sim applies them to the episodes it builds from now on and sends nothing back: the next step() carries
        the answer to its ACT as usual. Evaluation episodes stay evenly spread whatever the weights are.
        """
        payload = p.encode_weights(weights)
        self.sock.sendall(p.encode_header(p.MsgType.WEIGHTS, len(payload)) + payload)

    def set_replay(self, seed_base: int, fraction: float, seeds) -> None:
        """Evaluation seeds of `seed_base` that `fraction` of the training resets rebuild (see protocol REPLAY), in
        place of the ones sent before; no seeds or a fraction of 0 stops replaying. Nothing is sent back."""
        payload = p.encode_replay(seed_base, fraction, seeds)
        self.sock.sendall(p.encode_header(p.MsgType.REPLAY, len(payload)) + payload)

    def close(self) -> None:
        try:
            self.sock.sendall(p.encode_header(p.MsgType.CLOSE, 0))
        except OSError:
            pass
        self.sock.close()

    def __enter__(self) -> "ForgeEnv":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _receive_decision(self) -> p.Step:
        """Every group's STEP of one decision, joined in env order."""
        return p.join_steps([self._receive_step() for _ in self.groups])

    def _receive_step(self) -> p.Step:
        msg_type, length = self._receive_header()
        if msg_type != p.MsgType.STEP or length > len(self._step_buffer):
            raise ConnectionError(f"expected STEP of at most {len(self._step_buffer)} bytes, got type {msg_type} of "
                                  f"{length}")
        view = memoryview(self._step_buffer)[:length]
        self._read_into(view)
        step = p.decode_step(self.spec, view)
        if length != self.spec.step_payload_size(step.done.shape[0]):
            raise ConnectionError(f"STEP of {length} bytes does not hold the {step.done.shape[0]} envs it says")
        return step

    def _receive(self) -> tuple[int, bytes]:
        msg_type, length = self._receive_header()
        buffer = bytearray(length)
        self._read_into(memoryview(buffer))
        return msg_type, bytes(buffer)

    def _receive_header(self) -> tuple[int, int]:
        header = bytearray(p.HEADER.size)
        self._read_into(memoryview(header))
        return p.HEADER.unpack(header)

    def _read_into(self, view: memoryview) -> None:
        while len(view):
            got = self.sock.recv_into(view)
            if got == 0:
                raise ConnectionError("sim closed the connection")
            view = view[got:]


class ClusterEnv:
    """Several sims as one pool: the host's own and every worker's (AnimusForge.Cluster). Their envs are laid end to
    end in the order given, and every sim's groups are groups of the whole, so the pipelined rollout answers each
    sim's half as it comes and every sim's maps tick while the others' are decided. Whole-decision calls (step,
    reset, set_mode) go to every sim: an evaluation's seeds are shared out, each sim playing its own run of them.

    The sims must be one scenario: the same observation, state, action and episode info layouts, decision length
    and goals. How many envs each runs may differ.
    """

    def __init__(self, endpoints: list[str], connect_timeout: float = 600.0, rank: int = 0, ranks: int = 1):
        # The first sim is the host's, shared by every data-parallel learner; a worker's sim is one learner's alone.
        self.sims = [ForgeEnv(endpoint, connect_timeout, rank if index == 0 else 0, ranks if index == 0 else 1)
                     for index, endpoint in enumerate(endpoints)]
        first = self.sims[0].spec
        for endpoint, sim in zip(endpoints[1:], self.sims[1:]):
            mine = dataclasses.replace(sim.spec, num_envs=first.num_envs, env_groups=first.env_groups)
            if mine != first:
                raise ConnectionError(f"sim {endpoint} runs {sim.spec.scenario} with a different spec than "
                                      f"{endpoints[0]}'s {first.scenario}: a cluster's sims must be one scenario")

        self.offsets, self.groups, self._owners = [], [], []
        offset = 0
        for index, sim in enumerate(self.sims):
            self.offsets.append(offset)
            for begin, count in sim.groups:
                self.groups.append((offset + begin, count))
                self._owners.append((index, begin))
            offset += sim.spec.num_envs
        self.spec = dataclasses.replace(first, num_envs=offset, env_groups=len(self.groups))
        self._next_group = 0

    def _owner(self, begin: int) -> tuple[int, int]:
        return self._owners[[group for group, _ in self.groups].index(begin)]

    def _joined(self, parts: list[p.Step]) -> p.Step:
        return p.join_steps([dataclasses.replace(part, env_begin=part.env_begin + offset)
                             for part, offset in zip(parts, self.offsets)])

    def reset(self) -> p.Step:
        self._next_group = 0
        return self._joined([sim.reset() for sim in self.sims])

    def step(self, actions: np.ndarray, goals: np.ndarray | None = None) -> p.Step:
        # Every sim's answers go out before any STEP is read, so the sims tick together.
        for begin, count in self.groups:
            rows = slice(begin, begin + count)
            self.send_act(begin, actions[rows], goals[rows] if goals is not None else None)
        self._next_group = 0
        return self._joined([sim._receive_decision() for sim in self.sims])

    def send_act(self, env_begin: int, actions: np.ndarray, goals: np.ndarray | None = None) -> None:
        index, local = self._owner(env_begin)
        self.sims[index].send_act(local, actions, goals)

    def receive_step(self) -> p.Step:
        """The next group's STEP, in the order of `groups`: the pipelined rollout reads them in that order."""
        index, _ = self._owners[self._next_group]
        self._next_group = (self._next_group + 1) % len(self.groups)
        part = self.sims[index].receive_step()
        return dataclasses.replace(part, env_begin=part.env_begin + self.offsets[index])

    def set_mode(self, evaluate: bool, seed_base: int = 0, episodes: int = 0, baseline: str = "",
                 opponents_only: bool = False, first_seed: int = 0) -> p.Step:
        # An evaluation's seeds shared out in proportion to each sim's envs, in consecutive runs, so every seed is
        # played once and reported by its own index whichever sim plays it.
        shares = _shares(episodes, [sim.spec.num_envs for sim in self.sims]) if evaluate else [0] * len(self.sims)
        parts, start = [], first_seed
        for sim, share in zip(self.sims, shares):
            parts.append(sim.set_mode(evaluate, seed_base, share, baseline, opponents_only, start))
            start += share
        self._next_group = 0
        return self._joined(parts)

    def set_layout_weights(self, weights) -> None:
        for sim in self.sims:
            sim.set_layout_weights(weights)

    def set_replay(self, seed_base: int, fraction: float, seeds) -> None:
        for sim in self.sims:
            sim.set_replay(seed_base, fraction, seeds)

    def close(self) -> None:
        for sim in self.sims:
            sim.close()

    def __enter__(self) -> "ClusterEnv":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def _shares(total: int, weights: list[int]) -> list[int]:
    """`total` split in proportion to `weights`, whole numbers adding up to it."""
    whole = sum(weights) or 1
    shares = [total * weight // whole for weight in weights]
    for index in range(total - sum(shares)):
        shares[index % len(shares)] += 1
    return shares
