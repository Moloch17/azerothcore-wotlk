"""Asynchronous learners across machines (mappo.rank_sync = async): nobody waits for anybody.

Every rank trains on its own sim at its own pace -- its own rollouts, its own updates, on-policy on its own weights --
with no collective between them. The leader (rank 0, the host's learner) owns the run: it evaluates, decides,
checkpoints and writes, and its networks are the run's networks (the "centre"). A follower trades with it in the
background every `async_sync_every` updates:

    push   the follower's progress since the centre it last took (delta = its weights - that centre), the env steps
           it played meanwhile, and the training episodes it finished
    reply  the centre as it is now, and what the leader decided (entropy, learning-rate scale, the classes held out
           of training, layout weights and replay seeds for its sim, the run's env steps, whether to stop)

The leader folds a push in the next time its own update is idle: centre += alpha * delta, alpha = the follower's
steps over those plus every step the centre took since the follower's base (capped at MAX_ALPHA), which is the share
of the data behind the two -- elastic averaging, weighted by data. The follower, when the reply lands, keeps the
progress it made while the push was in flight and puts it on top of the new centre: weights = centre + (weights now -
weights pushed). A slow machine therefore costs the others nothing: it trades less often, and each trade carries
more steps.

The networks travel as one flat float32 vector: every parameter, then every floating-point buffer (the observation
normalisers' and the value normaliser's running statistics, which are part of what a network means). Only the
parameters are traded as deltas. The statistics are the leader's, and a follower takes them whole with each centre:
a running mean, variance and count moved by another rank's delta is no longer one -- a variance pushed below zero
made a square root of it NaN, and within two updates every weight of every rank was NaN. Length-prefixed pickles on
a plain TCP connection, on a trusted LAN like the rest of the cluster's links.

The leader also serves the checkpoints the run read -- the parents it seeded from, the teachers, the cast's agents
and its league -- and a follower fetches any it lacks before it sets up (fetch_shared), so every rank loads the same
files. Each machine's runs directory is its own, and the host's is the one those files are written to: until this, a
follower without them seeded nothing, distilled nothing and, in a stage whose owner is a cast checkpoint, died at
startup while the host trained alone (2026-09-27).
"""
from __future__ import annotations

import hashlib
import os
import pickle
import socket
import struct
import threading
import time
from pathlib import Path

import numpy as np
import torch

MAX_ALPHA = 0.5
_HEADER = struct.Struct("!Q")


# ---------------------------------------------------------------------- the vector

def _tensors(modules) -> list[torch.Tensor]:
    """What a network is, in a fixed order: every parameter of `modules`, then every floating-point buffer."""
    present = [module for module in modules if module is not None]
    return ([parameter for module in present for parameter in module.parameters()]
            + [buffer for module in present for buffer in module.buffers() if buffer.is_floating_point()])


def parameter_count(modules) -> int:
    """How much of flatten()'s vector is parameters (traded as deltas); the rest is the leader's statistics."""
    return sum(parameter.numel() for module in modules if module is not None for parameter in module.parameters())


def flatten(modules) -> np.ndarray:
    """The networks as one float32 vector, on the host."""
    with torch.no_grad():
        flat = torch.cat([tensor.detach().reshape(-1).float() for tensor in _tensors(modules)])
    return flat.cpu().numpy()


def assign(modules, vector: np.ndarray) -> None:
    """Load a vector flatten() made (of the same networks) back into them."""
    source = torch.from_numpy(vector)
    offset = 0
    with torch.no_grad():
        for tensor in _tensors(modules):
            size = tensor.numel()
            tensor.copy_(source[offset:offset + size].view_as(tensor).to(tensor.device, tensor.dtype))
            offset += size
    if offset != vector.size:
        raise ValueError(f"async sync: a vector of {vector.size} for networks of {offset}")


def mix(center: np.ndarray, delta: np.ndarray, pushed_steps: int, since_base: int) -> float:
    """Fold a follower's delta into the centre, in place; the weight it was given."""
    alpha = min(MAX_ALPHA, pushed_steps / max(1, pushed_steps + max(0, since_base)))
    center += alpha * delta
    return alpha


# ---------------------------------------------------------------------- the wire

def _send(sock: socket.socket, message: dict) -> None:
    payload = pickle.dumps(message, protocol=5)
    sock.sendall(_HEADER.pack(len(payload)) + payload)


def _receive(sock: socket.socket) -> dict:
    def exactly(count: int) -> bytes:
        chunks, left = [], count
        while left:
            chunk = sock.recv(min(left, 1 << 22))
            if not chunk:
                raise ConnectionError("async sync: the other side closed the connection")
            chunks.append(chunk)
            left -= len(chunk)
        return b"".join(chunks)

    (length,) = _HEADER.unpack(exactly(_HEADER.size))
    return pickle.loads(exactly(length))


def _connect(address: str, timeout: float) -> socket.socket:
    """A connection to the leader's hub, retried until `timeout`: the hub starts once the leader has set up."""
    host, _, port = address.rpartition(":")
    deadline = time.monotonic() + timeout
    while True:
        try:
            sock = socket.create_connection((host, int(port)), timeout=10.0)
            break
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(2.0)
    sock.settimeout(None)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return sock


# ---------------------------------------------------------------------- shared files

def digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 22), b""):
            sha.update(chunk)
    return sha.hexdigest()


def shared_listing(root: Path, paths) -> dict[str, Path]:
    """The files under `root` among `paths`, keyed by their path relative to it -- the name every machine shares, since
    each keeps its runs directory at its own place. Files elsewhere are not served."""
    root = root.resolve()
    listing: dict[str, Path] = {}
    for path in paths:
        if path is None:
            continue
        path = Path(path)
        if not path.is_file():
            continue
        try:
            relative = path.resolve().relative_to(root)
        except ValueError:
            continue
        listing[relative.as_posix()] = path.resolve()
    return listing


def fetch_shared(address: str, rank: int, root: Path, timeout: float) -> list[str]:
    """Copy into `root` every file the leader serves that this machine lacks or holds a different copy of: written
    beside, checked, then renamed over, so a reader never sees half a checkpoint. The names fetched."""
    sock = _connect(address, timeout)
    fetched: list[str] = []
    try:
        _send(sock, {"type": "files", "rank": rank})
        listing = _receive(sock).get("files", [])
        for relative, size, sha in listing:
            target = (root / relative).resolve()
            if root.resolve() not in target.parents:
                continue
            if target.is_file() and target.stat().st_size == size and digest(target) == sha:
                continue
            _send(sock, {"type": "fetch", "rank": rank, "path": relative})
            data = _receive(sock).get("data")
            if data is None or hashlib.sha256(data).hexdigest() != sha:
                print(f"Async learners: the leader could not serve {relative}; this rank goes without it", flush=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            partial = target.with_name(target.name + f".rank{rank}.part")
            partial.write_bytes(data)
            os.replace(partial, target)
            fetched.append(relative)
    finally:
        sock.close()
    if fetched:
        print(f"Async learners: rank {rank} fetched {len(fetched)} of the leader's files: {', '.join(fetched)}",
              flush=True)
    return fetched


# ---------------------------------------------------------------------- the leader

class Hub:
    """The leader's side: serves the centre and collects the followers' pushes, never touching the GPU itself. The
    run's own thread calls at_safe_point() while no update is running, which is where pushes are folded in and the
    centre that is served is taken again."""

    def __init__(self, address: str, modules, shared: dict[str, Path] | None = None):
        self.modules = modules
        self.parameters = parameter_count(modules)
        # The files a follower fetches before it sets up (fetch_shared), by their path under the runs directory.
        self.shared = dict(shared or {})
        self.digests: dict[str, tuple[float, int, str]] = {}
        self.lock = threading.Lock()
        self.inbox: list[dict] = []
        self.center = flatten(modules)
        self.center_steps = 0              # the run's env steps when the centre was taken
        self.control: dict = {"stop": False}
        self.traded = 0
        _, _, port = address.rpartition(":")
        self.server = socket.create_server(("0.0.0.0", int(port)), reuse_port=False)
        self.server.settimeout(1.0)
        self.closed = False
        threading.Thread(target=self._accept, name="async-hub", daemon=True).start()
        print(f"Async learners: serving the networks on port {port}", flush=True)

    def _accept(self) -> None:
        while not self.closed:
            try:
                conn, peer = self.server.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(target=self._serve, args=(conn, peer), daemon=True).start()

    def _reply(self) -> dict:
        with self.lock:
            return {"center": self.center, "center_steps": self.center_steps, "control": dict(self.control)}

    def _listing(self) -> list[tuple[str, int, str]]:
        """(name, size, sha256) of every shared file that still exists; a digest is kept until the file changes."""
        listing = []
        for relative, path in list(self.shared.items()):     # share() may add while this runs
            try:
                stat = path.stat()
            except OSError:
                continue
            cached = self.digests.get(relative)
            if cached is None or cached[:2] != (stat.st_mtime, stat.st_size):
                cached = (stat.st_mtime, stat.st_size, digest(path))
                self.digests[relative] = cached
            listing.append((relative, stat.st_size, cached[2]))
        return listing

    def _serve(self, conn: socket.socket, peer) -> None:
        rank = "?"
        joined = False          # a trading connection, not a follower fetching files before it sets up
        try:
            while True:
                message = _receive(conn)
                rank = message.get("rank", rank)
                kind = message["type"]
                joined = joined or kind in ("hello", "push")
                if kind == "files":
                    _send(conn, {"files": self._listing()})
                    continue
                if kind == "fetch":
                    # Only a file in the listing: the name is looked up, never joined onto a path.
                    path = self.shared.get(message.get("path", ""))
                    _send(conn, {"data": path.read_bytes() if path is not None and path.is_file() else None})
                    continue
                if kind == "push":
                    with self.lock:
                        self._post(message)
                elif kind == "hello":
                    print(f"Async learners: rank {rank} joined from {peer[0]}", flush=True)
                _send(conn, self._reply())
        except (ConnectionError, OSError, EOFError):
            if joined:
                print(f"Async learners: rank {rank} left", flush=True)
        finally:
            conn.close()

    def _post(self, push: dict) -> None:
        """File a push for the next safe point, one per rank. Until it is folded the leader keeps serving the same
        centre, so a rank's newer push is measured from the same base and already holds everything the older one
        did: it replaces it, and only the steps and episodes add up. Queued whole, the pushes piled up for as long
        as the leader was busy -- through a long evaluation five ranks' full network vectors grew its learner by
        0.7 GB a minute toward the OOM killer (stage17_party, 2026-09-28). Caller holds the lock."""
        for index, waiting in enumerate(self.inbox):
            if waiting["rank"] == push["rank"] and waiting["base_steps"] == push["base_steps"]:
                push["env_steps"] = int(push["env_steps"]) + int(waiting["env_steps"])
                push["episodes"] = list(waiting["episodes"]) + list(push["episodes"])
                push["layouts"] = list(waiting["layouts"]) + list(push["layouts"])
                self.inbox[index] = push
                return
        self.inbox.append(push)

    def set(self, **control) -> None:
        with self.lock:
            self.control.update(control)

    def share(self, listing: dict[str, Path]) -> None:
        """Serve more files (a new league member), and tell the followers there is something new to fetch: the
        control's `shared` count goes up, and a follower that sees it change fetches what it lacks (Run.apply_control).
        """
        with self.lock:
            fresh = {name: path for name, path in listing.items() if name not in self.shared}
            if not fresh:
                return
            self.shared.update(fresh)
            self.control["shared"] = int(self.control.get("shared", 0)) + 1

    def at_safe_point(self, run) -> None:
        """Fold in what the followers pushed, count their steps and episodes into the run, and take the centre that
        is served from now on. Only while no update is running on the networks."""
        with self.lock:
            inbox, self.inbox = self.inbox, []
        if inbox:
            center = flatten(self.modules)
            for push in inbox:
                steps = int(push["env_steps"])
                if not np.isfinite(push["delta"]).all():
                    # One machine gone wrong must not take the run with it.
                    print(f"Async learners: rank {push['rank']} pushed non-finite weights; its {steps} env steps are "
                          f"dropped", flush=True)
                    continue
                alpha = mix(center[:self.parameters], push["delta"], steps, run.env_steps - int(push["base_steps"]))
                run.env_steps += steps
                run.finished_episodes.extend(push["episodes"])
                run.finished_layouts.extend(push["layouts"])
                self.traded += 1
                if self.traded <= 3 or self.traded % 100 == 0:
                    print(f"Async learners: rank {push['rank']} traded {steps} env steps (weight {alpha:.2f}; "
                          f"{self.traded} trades so far)", flush=True)
            assign(self.modules, center)
            run.trainer.sync_rollout()
        else:
            center = flatten(self.modules)
        with self.lock:
            self.center = center
            self.center_steps = run.env_steps
            self.control.update(env_steps=run.env_steps, update=run.update)

    def close(self) -> None:
        """Tell the followers to stop (on their next trade), and stop listening a little later."""
        self.set(stop=True)
        time.sleep(2.0)
        self.closed = True
        self.server.close()


# ---------------------------------------------------------------------- a follower

class Link:
    """A follower's side: the first centre at startup, then a push every `every` updates on a thread of its own, and
    the reply folded in at a safe point of the run's own thread."""

    def __init__(self, address: str, rank: int, modules, every: int, timeout: float):
        self.rank = rank
        self.modules = modules
        self.parameters = parameter_count(modules)
        self.every = max(1, every)
        self.stopped = False
        self.control: dict = {}
        self.base = None                   # the centre this rank's weights are measured from
        self.base_steps = 0
        self.pushed = None                 # the weights as they were pushed, while a push is out
        self.reply = None
        self.updates = 0
        self.steps_since = 0
        self.outbox = None
        self.episodes: list = []           # the training episodes finished since the last push (observe)
        self.layouts: list[int] = []
        self.ready = threading.Condition()
        self.sock = _connect(address, timeout)

    def hello(self) -> dict:
        """The leader's networks and counters, loaded into this rank's: every rank starts as one network."""
        _send(self.sock, {"type": "hello", "rank": self.rank})
        reply = _receive(self.sock)
        assign(self.modules, reply["center"])
        self.base = reply["center"].copy()
        self.base_steps = int(reply["center_steps"])
        self.control = reply["control"]
        threading.Thread(target=self._trade, name="async-link", daemon=True).start()
        print(f"Async learners: rank {self.rank} took the leader's networks at {self.base_steps} env steps; trading "
              f"every {self.every} updates", flush=True)
        return reply["control"]

    def _trade(self) -> None:
        while True:
            with self.ready:
                while self.outbox is None and not self.stopped:
                    self.ready.wait()
                if self.stopped:
                    return
                message, self.outbox = self.outbox, None
            try:
                _send(self.sock, message)
                reply = _receive(self.sock)
            except (ConnectionError, OSError, EOFError):
                print(f"Async learners: rank {self.rank} lost the leader; stopping", flush=True)
                self.stopped = True
                return
            with self.ready:
                self.reply = reply

    def observe(self, episodes, layouts) -> None:
        """Training episodes that just ended, for the leader's controller (the run's own lists are the follower's log's,
        which clears them)."""
        self.episodes.extend(episodes)
        self.layouts.extend(int(layout) for layout in layouts)

    def at_safe_point(self, run) -> None:
        """Fold in a reply that has landed and push again when it is time (the run counts its env steps into
        steps_since as it plays them). Only while no update is running on the networks."""
        self.updates += 1
        with self.ready:
            reply, self.reply = self.reply, None
        if reply is not None:
            # The progress made while the push was out, on top of the centre's parameters; the leader's statistics.
            now = flatten(self.modules)
            center = reply["center"]
            rebased = center.copy()
            count = self.parameters
            rebased[:count] += now[:count] - self.pushed[:count]
            assign(self.modules, rebased)
            run.trainer.sync_rollout()
            self.base = center.copy()
            self.base_steps = int(reply["center_steps"])
            self.pushed = None
            self.control = reply["control"]
            run.apply_control(self.control)
            if self.control.get("stop"):
                self.stopped = True
        if self.pushed is None and self.updates >= self.every and not self.stopped:
            self.pushed = flatten(self.modules)
            count = self.parameters
            message = {"type": "push", "rank": self.rank, "delta": self.pushed[:count] - self.base[:count],
                       "env_steps": self.steps_since, "base_steps": self.base_steps,
                       "episodes": self.episodes, "layouts": self.layouts}
            self.episodes, self.layouts = [], []
            self.steps_since = 0
            self.updates = 0
            with self.ready:
                self.outbox = message
                self.ready.notify()

    def close(self) -> None:
        with self.ready:
            self.stopped = True
            self.ready.notify()
        try:
            self.sock.close()
        except OSError:
            pass
