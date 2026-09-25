"""Vectorised environment client for the Animus Forge sim.

The sim is the server and runs lock-step: it sends one STEP with every env's observations and
blocks until it gets one ACT back. All envs auto-reset inside the sim.
"""

from __future__ import annotations

import socket
import time

import numpy as np

from . import protocol as p


class ForgeEnv:
    def __init__(self, socket_path: str, connect_timeout: float = 600.0):
        self.socket_path = socket_path
        self.sock = self._connect(socket_path, connect_timeout)
        self.sock.sendall(p.encode_header(p.MsgType.HELLO, p.HELLO.size) + p.HELLO.pack(p.PROTOCOL_VERSION))

        msg_type, payload = self._receive()
        if msg_type != p.MsgType.SPEC:
            raise ConnectionError(f"expected SPEC, got message type {msg_type}")

        self.spec = p.decode_spec(payload)
        if self.spec.version != p.PROTOCOL_VERSION:
            raise ConnectionError(f"sim speaks protocol {self.spec.version}, client {p.PROTOCOL_VERSION}")

        self._step_buffer = bytearray(self.spec.step_payload_size())
        self._pending: p.Step | None = None

    @staticmethod
    def _connect(path: str, timeout: float) -> socket.socket:
        """Retry until the sim is listening: the server may still be loading the world."""
        deadline = time.monotonic() + timeout
        while True:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                sock.connect(path)
                return sock
            except (FileNotFoundError, ConnectionRefusedError):
                sock.close()
                if time.monotonic() > deadline:
                    raise
                time.sleep(1.0)

    def reset(self) -> p.Step:
        """The first STEP after connecting: freshly reset envs. Its reward/done are meaningless."""
        if self._pending is None:
            self._pending = self._receive_step()
        return self._pending

    def step(self, actions: np.ndarray, goals: np.ndarray | None = None) -> p.Step:
        """Send [E, A] actions, with the goals each agent is pursuing when the policy has a goal head, and return the
        next STEP. Goals are what the sim scores, reports and shows a party's teammates; they mask nothing."""
        actions = np.ascontiguousarray(actions, dtype="<i4")
        expected = (self.spec.num_envs, self.spec.agents_per_env)
        if actions.shape != expected:
            raise ValueError(f"actions must have shape {expected}, got {actions.shape}")

        payload = actions.tobytes()
        if goals is not None:
            goals = np.ascontiguousarray(goals, dtype="<i4")
            if goals.shape != expected:
                raise ValueError(f"goals must have shape {expected}, got {goals.shape}")
            payload += goals.tobytes()
        self.sock.sendall(p.encode_header(p.MsgType.ACT, len(payload)) + payload)
        self._pending = self._receive_step()
        return self._pending

    def set_mode(self, evaluate: bool, seed_base: int = 0, episodes: int = 0, baseline: str = "",
                 opponents_only: bool = False) -> p.Step:
        """Switch the sim between training and seeded evaluation (see protocol MODE). With `opponents_only` the
        baseline plays only the opponent seats of self-play episodes and the actions sent play the rest.

        Every env resets; the returned STEP holds the fresh observations and, like the first one, no transition.
        """
        payload = p.encode_mode(evaluate, seed_base, episodes, baseline, opponents_only)
        self.sock.sendall(p.encode_header(p.MsgType.MODE, len(payload)) + payload)
        self._pending = self._receive_step()
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

    def _receive_step(self) -> p.Step:
        msg_type, length = self._receive_header()
        if msg_type != p.MsgType.STEP or length != len(self._step_buffer):
            raise ConnectionError(f"expected STEP of {len(self._step_buffer)} bytes, got type {msg_type} of {length}")
        self._read_into(memoryview(self._step_buffer))
        return p.decode_step(self.spec, self._step_buffer)

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
