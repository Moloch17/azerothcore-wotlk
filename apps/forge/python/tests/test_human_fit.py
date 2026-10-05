"""animus.human.fit: the MoveBlock emulator and the beam search recovering a known action sequence."""

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


def test_the_emulator_follows_moveblock():
    space = fit.SPACES["lattice"]
    n = 6
    modes, speeds = np.zeros(n), np.full(n, 7.0)
    # Forward for a decision: 1.75 yards along the facing.
    path = fit.rollout(space, [act(space, fit.BEARING, 0)], {"x": 0, "y": 0, "facing": 0.0}, modes, speeds)
    assert path[1, :2] == pytest.approx([1.75, 0.0])
    # A quarter turn left takes two decisions at 45 degrees each.
    path = fit.rollout(space, [act(space, fit.TURN, fit.TURN_ANGLES[4]), act(space, fit.NOOP), act(space, fit.NOOP)],
                       {"x": 0, "y": 0, "facing": 0.0}, modes, speeds)
    assert path[1:, 3] == pytest.approx([math.pi / 4, math.pi / 2, math.pi / 2])
    # Bearing right is clockwise from the facing; back is at the back speed.
    path = fit.rollout(space, [act(space, fit.BEARING, 2)], {"x": 0, "y": 0, "facing": 0.0}, modes, speeds)
    assert path[1, :2] == pytest.approx([0.0, -1.75], abs=1e-9)
    path = fit.rollout(space, [act(space, fit.BEARING, 4)], {"x": 0, "y": 0, "facing": 0.0}, modes, speeds)
    assert path[1, 0] == pytest.approx(-4.5 * 0.25)
    # FACE_HEADING snaps the facing onto a held bearing and makes it forward.
    path = fit.rollout(space, [act(space, fit.BEARING, 2), act(space, fit.FACE_HEADING), act(space, fit.NOOP)],
                       {"x": 0, "y": 0, "facing": 0.0}, modes, speeds)
    assert motion.wrap(path[2, 3]) == pytest.approx(-math.pi / 2)
    assert path[3, 1] - path[2, 1] == pytest.approx(-1.75)
    # A jump carries the body along the facing at the run speed for its arc, feet cleared after.
    path = fit.rollout(space, [act(space, fit.JUMP)] + [act(space, fit.NOOP)] * 5, {"x": 0, "y": 0, "facing": 0.0},
                       modes, speeds)
    assert path[-1, 0] == pytest.approx(7.0 * fit.JUMP_SECONDS)
    # Pitch only off the ground: swimming forward at a 30 degree climb.
    swim = np.full(n, motion.MODE_SWIM)
    path = fit.rollout(space, [act(space, fit.PITCH, fit.PITCH_ANGLES[6]), act(space, fit.BEARING, 0)],
                       {"x": 0, "y": 0, "facing": 0.0}, swim, np.full(n, 4.72))
    assert path[1, 4] == pytest.approx(math.radians(30)) and path[2, 2] == pytest.approx(4.72 * 0.25 * 0.5)


def test_the_beam_search_recovers_a_known_sequence():
    space = fit.SPACES["lattice"]
    true = [act(space, fit.NOOP), act(space, fit.NOOP), act(space, fit.TURN, fit.TURN_ANGLES[4]), act(space, fit.NOOP),
            act(space, fit.NOOP), act(space, fit.BEARING, 2), act(space, fit.NOOP), act(space, fit.TURN,
                                                                                       fit.TURN_ANGLES[3]),
            act(space, fit.NOOP), act(space, fit.HALT), act(space, fit.NOOP), act(space, fit.BEARING, 7)]
    n = len(true) + 1
    start = {"x": 10.0, "y": -4.0, "facing": 1.0, "bearing": 0}
    path = fit.rollout(space, true, start, np.zeros(n), np.full(n, 7.0))
    human = as_samples(path, space.dt)
    got = fit.beam_fit(space, human, beam=32)
    assert got.pos_err.max() < 1e-6 and got.yaw_err.max() < 1e-6
    assert got.start_bearing == 0 and got.actions == true


def test_finer_turns_fit_a_gentle_arc_better_and_the_study_reports_every_space():
    # A run curving left at 20 degrees a second for 10 s, on the 125 ms grid.
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
    lattice = report["spaces"]["lattice"]["overall"]
    finer = report["spaces"]["lattice_fine"]["overall"]
    assert lattice["chunks"] == 2 and finer["heading_err_deg"]["mean"] <= lattice["heading_err_deg"]["mean"] + 1e-9
    assert "0" in report["spaces"]["turn_rate"]["contexts"]
    assert fit.markdown(report).count("| lattice |") >= 1
