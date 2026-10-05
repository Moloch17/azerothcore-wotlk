"""animus.human tracks -> segment -> dataset/reference/trips/hard spots on a synthetic hour of capture."""

import json
import math

import numpy as np
import pytest

import human_capture_writer as w
from animus.human import build, dataset, motion
from animus.human import reader as r
from animus.human import reference as ref
from animus.human import segment, tracks


@pytest.fixture()
def capture(tmp_path):
    root = tmp_path / "capture"
    facts = w.synthetic_hour(root)
    return root, facts


def shard_tracks(root, include_companions=False):
    cap = r.CaptureDir(root)
    out = []
    for sessions, shard in tracks.iter_shards(cap):
        out += build.shard_tracks(sessions, shard, include_companions)
    return out


def test_tracks_hold_stands_cut_at_teleports_flag_stuns_and_leave_companions_out(capture):
    root, facts = capture
    found = shard_tracks(root)
    assert {t.player for t in found} == {w.HUMAN}
    # The stand at A, the run east, the stand at B and the run north are one track: stands are held, not cut.
    first = found[0]
    t = first.samples[:, motion.T]
    assert t[0] == pytest.approx(w.T0 / 1000.0)
    assert np.allclose(np.diff(t), motion.DECISION_SECONDS)
    end_north = (facts["north_start"] + 13100) / 1000.0
    assert t[-1] == pytest.approx(end_north, abs=0.3)
    # The teleport cut: the far run is its own track, 5000 yards away.
    assert len(found) == 2 and found[1].samples[0, motion.X] == pytest.approx(5000.0, abs=1.0)
    # Stunned for a second: flagged, kept.
    stun = (t >= facts["stun"][0] / 1000.0) & (t <= facts["stun"][1] / 1000.0)
    assert stun.any() and (first.involuntary == stun).all()
    # Combat from the snapshots, mount none, speed the run speed, a jump counted where it was sent.
    north = (t >= facts["north_start"] / 1000.0 + 0.25) & (t < facts["north_start"] / 1000.0 + 7.5)
    assert (first.samples[north, motion.IN_COMBAT] == 1).all()
    assert (first.samples[t < facts["north_start"] / 1000.0 - 1, motion.IN_COMBAT] == 0).all()
    assert (first.samples[:, motion.SPEED] == 7.0).all() and first.jumps.sum() == 1
    assert first.info["level"] == 23 and first.latency_ms == 80
    with_companions = shard_tracks(root, include_companions=True)
    assert any(t.player == w.COMPANION and t.companion for t in with_companions)


def test_mode_comes_from_the_flags_flying_first():
    flags = np.array([0, tracks.MF_SWIMMING, tracks.MF_FLYING | tracks.MF_SWIMMING, tracks.MF_FALLING,
                      tracks.MF_FALLING | tracks.MF_SWIMMING, tracks.MF_CAN_FLY])
    assert list(tracks.flag_mode(flags)) == [0, 1, 2, 3, 1, 0]


def test_a_gap_that_is_not_a_stand_cuts_the_track():
    moves = np.zeros(4, r.PREFIX[r.MOVE][1])
    moves["ms"] = [0, 100, 5100, 5200]
    moves["player"] = 1
    moves["move_flags"] = [1, 0, 1, 1]
    moves["x"] = [0.0, 0.7, 30.0, 30.7]     # the packet after the gap is far away: a knockback or a lost stretch
    got = tracks.player_tracks(1, moves, np.zeros(0, r.PREFIX[r.MOTION_EVENT][1]), np.zeros(0, r.PREFIX[r.SPEEDS][1]))
    assert len(got) == 0 or all(t.samples[-1, motion.X] - t.samples[0, motion.X] < 5 for t in got)


def test_trips_end_where_the_player_stayed(capture):
    root, facts = capture
    first = shard_tracks(root)[0]
    interacts = r.read_all(next(root.rglob("action-0.bin.gz"))).get(r.INTERACT)
    found = segment.trips(first, interacts)
    east = [t for t in found if t.end[0] > 60]
    assert east, found
    trip = east[0]
    assert trip.start[:2] == pytest.approx(list(facts["a"]), abs=1.0)
    assert trip.end[:2] == pytest.approx(list(facts["b"]), abs=2.0)
    assert trip.destination in ("stay", "interact") and trip.mode == "ground"
    assert 9.0 <= trip.seconds <= 11.0 and len(trip.path) >= 5


def test_clips_leave_out_stuns_and_idle(capture):
    root, _ = capture
    first = shard_tracks(root)[0]
    clips = segment.clips(first)
    assert clips and all(not first.involuntary[c.start:c.end].any() for c in clips)
    assert any(c.combat_share > 0 for c in clips)
    standing = first.samples.copy()
    standing[:, motion.X:motion.Z + 1] = 0.0
    standing[:, motion.YAW] = 0.0
    idle = tracks.Track(1, 0, standing, np.zeros(len(standing), bool), np.zeros(len(standing), np.uint32),
                        np.zeros(len(standing), np.int32))
    assert segment.clips(idle) == []


def test_movement_metrics_count_reversals_stop_starts_and_strafes():
    dt = motion.DECISION_SECONDS
    # Turn left 30 degrees over two steps, pause one step, turn right 20: one reversal. Then a slow weave: none.
    steps = np.array([0.26, 0.26, 0.0, -0.18, -0.18, 0, 0, 0, 0, 0, 0, 0, 0.2])
    assert ref.turn_reversals(steps) == 1
    assert ref.turn_reversals(np.array([0.3, 0.3, 0, 0, 0, 0, 0, 0, -0.3])) == 0
    assert ref.turn_reversals(np.array([0.3, -math.pi + 0.001])) == 0
    # A strafe right then a stop and a restart within a second.
    pts = [(0.0, -i * 1.75, 0.0) for i in range(5)] + [(0.0, -7.0, 0.0)] * 2 + [(0.0, -7.0 - i * 1.75, 0.0)
                                                                            for i in range(1, 4)]
    samples = np.zeros((len(pts), motion.SAMPLE_DIM))
    for i, p in enumerate(pts):
        samples[i, :4] = [i * dt, *p]
    samples[:, motion.SPEED] = 7.0
    counts = ref.movement_counts(motion.features(samples))
    assert counts["strafe"] == counts["moving"] and counts["stop_starts"] == 1 and counts["backpedal"] == 0


def test_build_writes_the_four_files(capture, tmp_path, monkeypatch):
    monkeypatch.setattr(ref, "MIN_MOVING_MINUTES", 0.2)       # the synthetic hour moves for half a minute
    root, _ = capture
    out = tmp_path / "out"
    result = build.build(root, out, stride=1)
    assert result.tracks == 2 and result.windows > 0 and result.trips >= 1
    data = dataset.load(out / "human_motion_windows.npz")
    assert data["windows"].shape[1:] == (motion.WINDOW, motion.F)
    assert data["windows"].dtype == np.float32 and data["context"].dtype == np.int16
    assert len(data["weight"]) == len(data["context"]) and (data["weight"] == 1.0).all()
    assert data["meta"]["contexts"] and data["meta"]["source"]["players"] == 1
    assert set(data["context"].tolist()) <= {0, 1, 12, 13}
    reference = json.loads((out / "human_reference.json").read_text())
    assert reference["format"] == 1 and reference["step_seconds"] == 0.25
    assert reference["source"]["players"] == 1 and reference["source"]["hours"] == 1
    for context, entry in reference["motion"].items():
        assert entry["name"] == motion.context_name(int(context))
        for name in motion.HIST_FEATURES:
            assert len(entry["hist"][name]) == len(motion.HIST_BINS[name]) - 1
            assert sum(entry["hist"][name]) == entry["steps"]
    group = reference["metrics"]["mage/frost/20-29"]
    keys = list(group)
    assert keys[0] == "turn_reversals_per_min" and "casts_per_min" in keys and "cast_fail_rate" in keys
    assert group["cast_fail_rate"]["p50"] == pytest.approx(0.25)
    assert group["overheal_share"]["p50"] == pytest.approx(1 / 3, abs=1e-4)
    assert group["dps"]["p50"] == pytest.approx(900 / 8, rel=1e-3)
    assert "mage/all/all" in reference["metrics"] and "all/all/all" in reference["metrics"]
    trips = json.loads((out / "human_trips.json").read_text())
    assert set(trips) == {"format", "maps"} and trips["maps"]["0"]
    assert all(set(t) == {"start", "end", "seconds", "mode", "path"} for t in trips["maps"]["0"])
    spots = json.loads((out / "human_hard_spots.json").read_text())
    assert set(spots) == {"format", "maps"} and any(s["kind"] == "fall" for s in spots["maps"]["0"])
    assert all(set(s) == {"pos", "kind", "count"} for s in spots["maps"]["0"])


def test_the_dataset_keeps_a_bounded_uniform_sample():
    builder = dataset.DatasetBuilder(max_windows=50)
    s = np.zeros((40, motion.SAMPLE_DIM))
    s[:, motion.T] = np.arange(40) * 0.25
    s[:, motion.X] = np.arange(40) * 1.75
    s[:, motion.SPEED] = 7.0
    for _ in range(10):
        builder.add_clip(s, latency_ms=400.0)
    windows, context, weight = builder.arrays()
    assert len(windows) == 50 and builder.seen[0] == 10 * (39 - motion.WINDOW + 1)
    assert weight[0] == pytest.approx(dataset.latency_weight(400.0)) and weight[0] < 1.0


def test_realism_is_zero_against_itself(capture, tmp_path):
    root, _ = capture
    out = tmp_path / "out"
    build.build(root, out, stride=1)
    data = dataset.load(out / "human_motion_windows.npz")
    reference = json.loads((out / "human_reference.json").read_text())
    score = ref.realism(reference, data["windows"][:, -1:, :])
    assert score["contexts"] and np.isfinite(score["mean_emd"]) and score["mean_emd"] < 0.2
