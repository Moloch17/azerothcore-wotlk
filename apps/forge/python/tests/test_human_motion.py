"""animus.human.motion: one definition of motion features for humans and bots (FORMAT.md §3)."""

import math

import numpy as np
import pytest

from animus.human import motion as m


def track(points, yaw, mode=m.MODE_GROUND, speed=7.0, pitch=0.0, combat=0, mounted=0, step=m.DECISION_SECONDS):
    """Samples on the decision grid from [(x, y, z)] and a yaw per point (or one for all)."""
    yaws = np.broadcast_to(np.asarray(yaw, dtype=np.float64), (len(points),))
    out = np.zeros((len(points), m.SAMPLE_DIM))
    for i, (x, y, z) in enumerate(points):
        out[i] = [i * step, x, y, z, yaws[i], pitch, mode, mounted, speed, combat]
    return out


def test_running_forward_at_full_speed_is_fwd_one_whichever_way_the_body_faces():
    for yaw in (0.0, 1.0, -2.5):
        pts = [(i * 1.75 * math.cos(yaw), i * 1.75 * math.sin(yaw), 0.0) for i in range(5)]
        f = m.features(track(pts, yaw))
        assert f.shape == (4, m.F)
        np.testing.assert_allclose(f[:, m.INDEX["fwd"]], 1.0, atol=1e-5)
        np.testing.assert_allclose(f[:, m.INDEX["lat"]], 0.0, atol=1e-5)
        np.testing.assert_allclose(f[:, m.INDEX["yaw_rate"]], 0.0, atol=1e-5)
        np.testing.assert_allclose(f[:, m.INDEX["course_cos"]], 1.0, atol=1e-5)
        assert (f[:, m.INDEX["moving"]] == 1).all()


def test_strafing_left_and_turning_on_the_spot():
    strafe = m.features(track([(0.0, i * 1.75, 0.0) for i in range(4)], 0.0))
    np.testing.assert_allclose(strafe[:, m.INDEX["lat"]], 1.0, atol=1e-5)
    np.testing.assert_allclose(strafe[:, m.INDEX["course_sin"]], 1.0, atol=1e-5)
    # 90 degrees a second on the spot: half of pi a second, planar zero, course undefined -> zero.
    turn = m.features(track([(0.0, 0.0, 0.0)] * 5, [i * math.pi / 8 for i in range(5)]))
    np.testing.assert_allclose(turn[:, m.INDEX["yaw_rate"]], 0.5, atol=1e-5)
    np.testing.assert_allclose(turn[:, m.INDEX["planar"]], 0.0, atol=1e-6)
    assert (turn[:, m.INDEX["course_sin"]] == 0).all() and (turn[:, m.INDEX["moving"]] == 0).all()
    # A turn across the wrap is the short way round.
    wrapped = m.features(track([(0.0, 0.0, 0.0)] * 2, [math.pi - 0.1, -math.pi + 0.1]))
    assert wrapped[0, m.INDEX["yaw_rate"]] == pytest.approx(0.2 / 0.25 / math.pi, rel=1e-4)


def test_speed_is_relative_to_the_speed_in_force_and_modes_are_one_hot():
    swim = m.features(track([(i * 1.175, 0.0, -i * 0.5) for i in range(3)], 0.0, mode=m.MODE_SWIM, speed=4.7))
    np.testing.assert_allclose(swim[:, m.INDEX["fwd"]], 1.0, atol=1e-5)
    np.testing.assert_allclose(swim[:, m.INDEX["up"]], -0.5 / 0.25 / 4.7, atol=1e-5)
    assert (swim[:, m.INDEX["mode_swim"]] == 1).all() and (swim[:, m.INDEX["mode_ground"]] == 0).all()
    ctx = m.step_contexts(track([(0, 0, 0)] * 3, 0.0, mode=m.MODE_FLY, mounted=1, combat=1))
    assert (ctx == 2 * 4 + 2 + 1).all() and m.context_name(int(ctx[0])) == "fly_mounted_combat"


def test_resample_puts_irregular_packets_on_the_grid_and_cuts_at_gaps():
    # Packets at irregular times along a straight east run at 7 yd/s, yaw crossing the wrap, then a 3 s gap.
    times = [0.0, 0.1, 0.43, 0.5, 0.9, 1.0, 4.0, 4.3, 4.6]
    raw = np.zeros((len(times), m.SAMPLE_DIM))
    for i, t in enumerate(times):
        raw[i] = [t, 7.0 * t, 0.0, 0.0, (math.pi - 0.05 + 0.1 * t) % (2 * math.pi), 0.0, 0, 0, 7.0, 0]
    tracks = m.resample(raw)
    assert len(tracks) == 2
    first = tracks[0]
    np.testing.assert_allclose(first[:, m.T], [0.0, 0.25, 0.5, 0.75, 1.0])
    np.testing.assert_allclose(first[:, m.X], 7.0 * first[:, m.T], atol=1e-6)
    # Yaw interpolated along the short arc across pi, not back through zero.
    assert abs(m.wrap(first[2, m.YAW] - (math.pi - 0.05 + 0.05))) < 1e-6


def test_windows_and_histogram_distance():
    f = m.features(track([(i * 1.75, 0.0, 0.0) for i in range(12)], 0.0))
    ctx = m.step_contexts(track([(i * 1.75, 0.0, 0.0) for i in range(12)], 0.0))
    w, c = m.windows(f, ctx)
    assert w.shape == (11 - m.WINDOW + 1, m.WINDOW, m.F) and c.shape == (w.shape[0],)
    hist = m.histograms(f, ctx)
    bins = m.HIST_BINS["fwd"]
    assert m.histogram_distance(hist[0]["fwd"], hist[0]["fwd"], bins) == 0.0
    shifted = np.roll(hist[0]["fwd"], 5)
    assert m.histogram_distance(hist[0]["fwd"], shifted, bins) == pytest.approx(5 * (bins[1] - bins[0]))
    assert math.isnan(m.histogram_distance(np.zeros(60), hist[0]["fwd"], bins))
