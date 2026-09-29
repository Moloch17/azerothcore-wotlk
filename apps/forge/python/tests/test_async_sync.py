"""animus.async_sync: the vector, the mixing rule, and a leader and a follower trading over a real socket."""
from __future__ import annotations

import socket
import time

import numpy as np
import torch

from animus.async_sync import MAX_ALPHA, Hub, Link, assign, fetch_shared, flatten, mix, shared_listing


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Normaliser(torch.nn.Module):
    def __init__(self):
        super().__init__()
        self.register_buffer("mean", torch.zeros(3))
        self.register_buffer("count", torch.zeros((), dtype=torch.long))   # not floating point: not traded


class FakeTrainer:
    def __init__(self):
        self.synced = 0

    def sync_rollout(self):
        self.synced += 1


class FakeRun:
    def __init__(self):
        self.env_steps = 0
        self.update = 0
        self.finished_episodes = []
        self.finished_layouts = []
        self.trainer = FakeTrainer()
        self.controls = []

    def apply_control(self, control):
        self.controls.append(control)


def networks(seed: int):
    torch.manual_seed(seed)
    return [torch.nn.Linear(4, 3), torch.nn.Linear(3, 2), Normaliser()]


def test_flatten_assign_round_trip_includes_float_buffers_only():
    modules = networks(0)
    modules[2].mean.fill_(2.5)
    vector = flatten(modules)
    assert vector.size == 4 * 3 + 3 + 3 * 2 + 2 + 3
    other = networks(1)
    assign(other, vector)
    np.testing.assert_array_equal(flatten(other), vector)
    assert float(other[2].mean[0]) == 2.5


def test_mix_weights_by_data_and_caps():
    center = np.zeros(3, dtype=np.float32)
    alpha = mix(center, np.ones(3, dtype=np.float32), pushed_steps=100, since_base=300)
    assert alpha == 0.25
    np.testing.assert_allclose(center, 0.25)
    assert mix(np.zeros(1, dtype=np.float32), np.ones(1, dtype=np.float32), 1000, 0) == MAX_ALPHA


def wait_for(condition, seconds=10.0):
    deadline = time.monotonic() + seconds
    while not condition():
        assert time.monotonic() < deadline, "timed out"
        time.sleep(0.01)


def test_leader_and_follower_trade():
    address = f"127.0.0.1:{free_port()}"
    leader_nets, follower_nets = networks(0), networks(1)
    hub = Hub(address, leader_nets)
    leader_run, follower_run = FakeRun(), FakeRun()
    hub.set(entropy_coef=0.01)

    link = Link(address, 1, follower_nets, every=1, timeout=10.0)
    control = link.hello()
    # The follower starts as the leader's networks.
    np.testing.assert_array_equal(flatten(follower_nets), flatten(leader_nets))
    assert control["entropy_coef"] == 0.01

    # The follower trains (moves its weights by +1) over 400 steps and pushes at its safe point.
    start = flatten(follower_nets)
    assign(follower_nets, start + 1.0)
    link.steps_since = 400
    link.observe([np.ones(2)], [0])
    link.at_safe_point(follower_run)
    wait_for(lambda: hub.inbox)

    # The leader played 400 steps of its own since the follower's base, then folds the push in: alpha 0.5, on the
    # parameters only -- the normaliser's statistics (the last 3) are the leader's.
    leader_run.env_steps = 400
    before = flatten(leader_nets)
    hub.at_safe_point(leader_run)
    after = flatten(leader_nets)
    np.testing.assert_allclose(after[:-3], before[:-3] + 0.5, rtol=0, atol=1e-6)
    np.testing.assert_array_equal(after[-3:], before[-3:])
    assert leader_run.env_steps == 800
    assert len(leader_run.finished_episodes) == 1 and leader_run.finished_layouts == [0]
    assert leader_run.trainer.synced == 1

    # The follower's reply lands (it carries the centre as it was when served); meanwhile it moved +2 more.
    wait_for(lambda: link.reply is not None)
    served = link.reply["center"].copy()
    assign(follower_nets, start + 3.0)
    link.at_safe_point(follower_run)
    rebased = flatten(follower_nets)
    np.testing.assert_allclose(rebased[:-3], served[:-3] + 2.0, rtol=0, atol=1e-6)
    np.testing.assert_array_equal(rebased[-3:], served[-3:])     # the leader's statistics, taken whole
    assert follower_run.controls and follower_run.trainer.synced == 1

    hub.close()
    link.close()


def test_stop_reaches_the_follower():
    address = f"127.0.0.1:{free_port()}"
    hub = Hub(address, networks(0))
    link = Link(address, 1, networks(1), every=1, timeout=10.0)
    link.hello()
    hub.set(stop=True)
    run = FakeRun()
    link.at_safe_point(run)             # pushes
    wait_for(lambda: link.reply is not None)
    link.at_safe_point(run)             # takes the reply, which says stop
    assert link.stopped
    hub.close()
    link.close()


def test_statistics_are_never_traded_as_deltas():
    """A variance moved by another rank's delta can go negative (the stage 8 NaN): statistics come whole."""
    address = f"127.0.0.1:{free_port()}"
    leader_nets, follower_nets = networks(0), networks(1)
    hub = Hub(address, leader_nets)
    link = Link(address, 1, follower_nets, every=1, timeout=10.0)
    link.hello()
    follower_nets[2].mean.fill_(-50.0)          # the follower's own statistics drift far
    link.steps_since = 100
    link.at_safe_point(FakeRun())
    wait_for(lambda: hub.inbox)
    assert hub.inbox[0]["delta"].size == 4 * 3 + 3 + 3 * 2 + 2   # parameters only
    run = FakeRun()
    hub.at_safe_point(run)
    assert float(leader_nets[2].mean[0]) == 0.0
    hub.close()
    link.close()


def test_a_follower_fetches_what_it_lacks_and_keeps_what_it_has(tmp_path):
    leader_root, follower_root = tmp_path / "leader", tmp_path / "follower"
    owner = leader_root / "stage6_gauntlet" / "best.pt"
    league = leader_root / "stage14_duel_pvp" / "league" / "step_1.pt"
    for path, body in ((owner, b"owner" * 1000), (league, b"member")):
        path.parent.mkdir(parents=True)
        path.write_bytes(body)
    outside = tmp_path / "elsewhere.pt"
    outside.write_bytes(b"not served")
    # One the follower already has, byte for byte, and one it has a stale copy of.
    (follower_root / "stage14_duel_pvp" / "league").mkdir(parents=True)
    (follower_root / "stage14_duel_pvp" / "league" / "step_1.pt").write_bytes(b"member")
    (follower_root / "stage6_gauntlet").mkdir(parents=True)
    (follower_root / "stage6_gauntlet" / "best.pt").write_bytes(b"old")

    listing = shared_listing(leader_root, [owner, league, outside, leader_root / "missing.pt", None])
    assert sorted(listing) == ["stage14_duel_pvp/league/step_1.pt", "stage6_gauntlet/best.pt"]

    address = f"127.0.0.1:{free_port()}"
    hub = Hub(address, networks(0), listing)
    try:
        fetched = fetch_shared(address, 1, follower_root, timeout=10.0)
        assert fetched == ["stage6_gauntlet/best.pt"]
        assert (follower_root / "stage6_gauntlet" / "best.pt").read_bytes() == owner.read_bytes()
        assert not any(path.name.endswith(".part") for path in follower_root.rglob("*"))
        # Nothing left to take the second time.
        assert fetch_shared(address, 1, follower_root, timeout=10.0) == []
    finally:
        hub.close()


def test_sharing_a_new_league_member_tells_the_followers(tmp_path):
    root = tmp_path / "leader"
    first = root / "stage14_duel_pvp" / "league" / "step_1.pt"
    first.parent.mkdir(parents=True)
    first.write_bytes(b"first")
    address = f"127.0.0.1:{free_port()}"
    hub = Hub(address, networks(0), shared_listing(root, [first]))
    try:
        assert hub.control.get("shared", 0) == 0
        hub.share(shared_listing(root, [first]))            # nothing new: nothing to announce
        assert hub.control.get("shared", 0) == 0
        second = first.with_name("step_2.pt")
        second.write_bytes(b"second")
        hub.share(shared_listing(root, [first, second]))
        assert hub.control["shared"] == 1
        follower = tmp_path / "follower"
        assert sorted(fetch_shared(address, 1, follower, timeout=10.0)) == [
            "stage14_duel_pvp/league/step_1.pt", "stage14_duel_pvp/league/step_2.pt"]
    finally:
        hub.close()


def test_a_ranks_pushes_while_the_leader_is_busy_are_kept_as_one():
    hub = Hub(f"127.0.0.1:{free_port()}", networks(0))
    try:
        size = hub.parameters
        push = lambda rank, value, steps, base=0: {"type": "push", "rank": rank, "delta": np.full(size, value, np.float32),
                                                   "env_steps": steps, "base_steps": base, "episodes": [steps],
                                                   "layouts": [rank]}
        with hub.lock:
            hub._post(push(1, 1.0, 100))
            hub._post(push(2, 5.0, 50))
            hub._post(push(1, 2.0, 30))      # the same base: supersedes rank 1's first
            hub._post(push(1, 3.0, 7, base=999))  # a new base: its own entry
        assert [(p["rank"], p["env_steps"], float(p["delta"][0])) for p in hub.inbox] == [
            (1, 130, 2.0), (2, 50, 5.0), (1, 7, 3.0)]
        assert hub.inbox[0]["episodes"] == [100, 30]
    finally:
        hub.close()
