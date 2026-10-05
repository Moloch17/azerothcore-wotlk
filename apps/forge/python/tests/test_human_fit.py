"""animus.human.fit: the player controller's emulator and the beam search recovering a known action sequence."""

import math

import numpy as np
import pytest

from animus.human import fit, motion


def act(space, kind, value=0.0):
    for i, (k, v) in enumerate(space.actions):
        if k == kind and abs(v - value) < 1e-6:
            return i
    raise KeyError((kind, value))


def as_samples(path, dt, mode=motion.MODE_GROUND, speed=7.0):
    out = np.zeros((len(path), motion.SAMPLE_DIM))
    out[:, motion.T] = np.arange(len(path)) * dt
    out[:, motion.X:motion.Z + 1] = path[:, :3]
    out[:, motion.YAW] = path[:, 3]
    out[:, motion.PITCH] = path[:, 4]
    out[:, motion.MODE] = mode
    out[:, motion.SPEED] = speed
    return out


def by_name(space, name):
    return next(i for i in range(len(space.actions)) if space.label(i) == name)


def test_the_emulator_follows_the_controller():
    space = fit.SPACES["controller"]
    n = 8
    modes, speeds = np.zeros(n), np.full(n, 7.0)
    origin = {"x": 0, "y": 0, "facing": 0.0}
    a = lambda name: by_name(space, name)  # noqa: E731
    # Forward for a decision: 1.75 yards along the facing; held, it keeps going.
    path = fit.rollout(space, [a("move_forward"), a("noop")], origin, modes, speeds)
    assert path[1, :2] == pytest.approx([1.75, 0.0]) and path[2, :2] == pytest.approx([3.5, 0.0])
    # A held turn rate: 90 deg/s left is an eighth of a turn a decision, until turn_stop.
    path = fit.rollout(space, [a("turn_left_90"), a("noop"), a("turn_stop"), a("noop")], origin, modes, speeds)
    assert path[1:, 3] == pytest.approx([math.pi / 8, math.pi / 4, math.pi / 4, math.pi / 4])
    # Strafe right is clockwise of the facing; back is at the back speed; a diagonal is x 0.7071 each way.
    path = fit.rollout(space, [a("strafe_right")], origin, modes, speeds)
    assert path[1, :2] == pytest.approx([0.0, -1.75], abs=1e-9)
    path = fit.rollout(space, [a("move_back")], origin, modes, speeds)
    assert path[1, 0] == pytest.approx(-4.5 * 0.25)
    path = fit.rollout(space, [a("move_forward"), a("strafe_left")], origin, modes, speeds)
    assert path[2, :2] - path[1, :2] == pytest.approx([1.75 * fit.DIAGONAL, 1.75 * fit.DIAGONAL])
    # Walking caps the speed at the walk's.
    path = fit.rollout(space, [a("walk_toggle"), a("move_forward")], origin, modes, speeds)
    assert path[2, 0] == pytest.approx(2.5 * 0.25)
    # A jump freezes the velocity it launched with for its arc: letting go of forward mid-air changes nothing.
    path = fit.rollout(space, [a("move_forward"), a("jump"), a("move_stop")] + [a("noop")] * 5, origin, modes, speeds)
    assert path[-1, 0] == pytest.approx(1.75 + 7.0 * fit.JUMP_SECONDS)
    # Swimming: the look direction is forward, pitched by a held pitch rate; ascend climbs at 45 degrees.
    swim, slow = np.full(n, motion.MODE_SWIM), np.full(n, 4.72)
    path = fit.rollout(space, [a("pitch_up_90"), a("pitch_stop"), a("move_forward")], origin, swim, slow)
    assert path[1, 4] == pytest.approx(math.radians(22.5))
    assert path[3, 2] == pytest.approx(4.72 * 0.25 * math.sin(math.radians(22.5)))
    path = fit.rollout(space, [a("ascend")], origin, swim, slow)
    assert path[1, 2] == pytest.approx(4.72 * 0.25 * fit.VERTICAL_SHARE) and path[1, 0] == pytest.approx(0.0)
    # Pitching and climbing are refused on the ground (MoveControls::Allowed); their stops are not.
    state = fit.State.start(4, 0, 0, 0, 0.0)
    kinds = np.array([fit.PITCH, fit.VERTICAL, fit.PITCH, fit.VERTICAL])
    values = np.array([math.radians(30), 1.0, 0.0, 0.0])
    _, allowed = fit.step(state, kinds, values, motion.MODE_GROUND, 7.0, space)
    assert allowed.tolist() == [False, False, True, True]


def test_the_beam_search_recovers_a_known_sequence():
    space = fit.SPACES["controller"]
    a = lambda name: by_name(space, name)  # noqa: E731
    true = [a("noop"), a("noop"), a("turn_left_90"), a("noop"), a("turn_stop"), a("strafe_right"), a("noop"),
            a("turn_right_30"), a("noop"), a("move_stop"), a("noop"), a("strafe_stop"), a("move_back"),
            a("walk_toggle")]
    n = len(true) + 1
    start = {"x": 10.0, "y": -4.0, "facing": 1.0, "feet": (1, 0)}
    path = fit.rollout(space, true, start, np.zeros(n), np.full(n, 7.0))
    human = as_samples(path, space.dt)
    got = fit.beam_fit(space, human, beam=32)
    assert got.pos_err.max() < 1e-6 and got.yaw_err.max() < 1e-6
    assert got.start_feet == (1, 0) and got.actions == true


def test_a_gentle_arc_fits_and_the_study_reports_every_space():
    # A run curving left at 20 degrees a second for 10 s, on the 125 ms grid: the policy has 30 and 0 deg/s.
    dt = 0.125
    t = np.arange(0, 10.0 + 1e-9, dt)
    rate = math.radians(20)
    yaw = rate * t
    x = 7.0 * np.sin(yaw) / rate
    y = 7.0 * (1 - np.cos(yaw)) / rate
    fine = np.zeros((len(t), motion.SAMPLE_DIM))
    fine[:, motion.T] = t
    fine[:, motion.X], fine[:, motion.Y], fine[:, motion.YAW] = x, y, yaw
    fine[:, motion.SPEED] = 7.0
    study = fit.FitStudy(beam=16)
    study.add_clip(fine)
    report = study.report()
    assert set(report["spaces"]) == set(fit.SPACES)
    coarse = report["spaces"]["controller"]["overall"]
    finer = report["spaces"]["controller_125ms"]["overall"]
    assert coarse["chunks"] == 2 and coarse["heading_err_deg"]["mean"] < 5.0
    assert finer["heading_err_deg"]["mean"] <= coarse["heading_err_deg"]["mean"] + 1e-9
    assert "0" in report["spaces"]["controller"]["contexts"]
    assert fit.markdown(report).count("| controller |") >= 1
