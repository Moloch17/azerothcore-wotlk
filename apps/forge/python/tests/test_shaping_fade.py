"""peak-play W1: the shaping ladder steps on evidence, carries across a resume, and reaches the sim (protocol 18)."""

import struct

import pytest

from animus import protocol as p
from animus.config import FadeConfig, TrainConfig, from_dict
from animus.stage import ConvergenceController, ShapingFade, restore_evaluation_state


def _fade(**fade) -> ShapingFade:
    config = TrainConfig()
    config.fade = FadeConfig(**{"enabled": True, "rungs": (1.0, 0.5, 0.0), "window": 2, "give_up": 2, **fade})
    return ShapingFade(config)


def _play(fade: ShapingFade, scores, settled=True, anneal_at=None) -> list[float]:
    """Feed evaluations (score, stderr 0.1, a step apart); the scale after each."""
    scales = []
    for index, score in enumerate(scores):
        fade.observe(score, 0.1, (index + 1) * 10, settled, anneal_starting=anneal_at == index)
        scales.append(fade.scale)
    return scales


@pytest.mark.parametrize("case, scores, settled, anneal_at, expected", [
    # A flat score plateaus once `window` evaluations after the rung's first set no new best, and steps; flat again,
    # it steps to the last rung and floors there.
    ("plateau", [5, 5, 5, 5, 5, 5, 5, 5], True, None, [1, 1, 0.5, 0.5, 0.5, 0, 0, 0]),
    # A difficulty ladder still moving holds it, however flat the score.
    ("ladder moving", [5, 5, 5, 5, 5], False, None, [1, 1, 1, 1, 1]),
    # Not at the evaluation where the learning-rate anneal starts; the next one may.
    ("anneal starting", [5, 5, 5, 5], True, 2, [1, 1, 1, 0.5]),
    # A climbing score is not a plateau.
    ("climbing", [1, 2, 3, 4, 5, 6], True, None, [1, 1, 1, 1, 1, 1]),
])
def test_the_ladder_steps_down_only_on_a_settled_plateau(case, scores, settled, anneal_at, expected):
    assert _play(_fade(), scores, settled, anneal_at) == pytest.approx(expected), case


def test_a_regression_steps_back_up_waits_and_is_held_after_give_up():
    fade = _fade()
    assert _play(fade, [5, 5]) == [1, 1]
    assert fade.observe(5, 0.1, 30) and fade.scale == 0.5           # plateau: down to 0.5, step score 5
    # Within regress_z (2 x sqrt(0.1^2 + 0.1^2) = 0.28) of the step: stays.
    assert fade.observe(4.8, 0.1, 50) is None and fade.scale == 0.5
    message = fade.observe(4.0, 0.1, 60)                           # 1.0 below: back up
    assert fade.scale == 1.0 and "steps back up" in message and "x0.5 -> x1" in message
    assert fade.falls == {0: 1} and not fade.held and not fade.settled
    # The wait: no step for `window` evaluations, however flat.
    assert fade.observe(5, 0.1, 70) is None and fade.scale == 1.0
    fade.observe(5, 0.1, 80)
    fade.observe(5, 0.1, 90)
    assert fade.scale == 0.5                                        # tried again
    fade.observe(3.0, 0.1, 100)                                     # and fell again: held at full shaping
    assert fade.scale == 1.0 and fade.held and fade.settled
    assert _play(fade, [5] * 6) == [1] * 6                          # held means held

    # Disabled, it is never anything but 1, whatever the scores do.
    off = _fade(enabled=False)
    assert _play(off, [5] * 6 + [0] * 3) == [1] * 9 and off.settled


def test_state_round_trips_mid_wait_and_survives_resumes_across_configs():
    fade = _fade()
    _play(fade, [5, 5, 5, 5, 4.0, 5])                               # down, back up, one evaluation into the wait
    config = TrainConfig()
    config.fade = FadeConfig(enabled=True, rungs=(1.0, 0.5, 0.0), window=2)
    controller = ConvergenceController(config, ["mage_dps"])
    controller.fade = fade
    saved = {"convergence": controller.tracker.state_dict(), "controller": controller.state_dict(),
             "score_kind": "score_outcome"}

    again = ConvergenceController(config, ["mage_dps"])
    assert restore_evaluation_state(again.tracker, again, saved, "score_outcome") is None
    assert again.fade.state_dict() == fade.state_dict()
    # The wait carries on where it was: one more flat evaluation is not yet a step, the one after may be.
    assert again.fade.observe(5, 0.1, 70) is None and again.fade.scale == 1.0

    # Resumed with the fade off: shaping in full, whatever rung the checkpoint was on.
    off = TrainConfig()
    off_controller = ConvergenceController(off, ["mage_dps"])
    restore_evaluation_state(off_controller.tracker, off_controller, saved, "score_outcome")
    assert off_controller.fade.scale == 1.0
    # A checkpoint from before the fade (no "fade" state) resumed with it on starts at the top rung.
    old = {**saved, "controller": {key: value for key, value in saved["controller"].items() if key != "fade"}}
    fresh = ConvergenceController(config, ["mage_dps"])
    restore_evaluation_state(fresh.tracker, fresh, old, "score_outcome")
    assert fresh.fade.rung == 0 and fresh.fade.scale == 1.0
    # Fewer rungs configured than the checkpoint's rung: the last one, not an index error.
    deep = _fade(rungs=(1.0, 0.75, 0.5, 0.0))
    deep.rung = 3
    short = _fade(rungs=(1.0, 0.0))
    short.load_state_dict(deep.state_dict())
    assert short.scale == 0.0
    # Scores of another kind drop what was measured but keep the rung.
    kind = ConvergenceController(config, ["mage_dps"])
    restore_evaluation_state(kind.tracker, kind, {**saved, "score_kind": ""}, "score_outcome")
    assert kind.fade.rung == fade.rung and kind.fade.step_score is None and kind.fade.tracker.history == []


@pytest.mark.parametrize("raw, key", [
    ({"rungs": [1.0, 0.5]}, "fade.rungs"),            # does not end at 0
    ({"rungs": [1.0, 1.0, 0.0]}, "fade.rungs"),       # not strictly falling
    ({"rungs": [1.5, 0.0]}, "fade.rungs"),            # above 1
    ({"rungs": []}, "fade.rungs"),
    ({"window": 0}, "fade.window"),
    ({"regress_z": 0.0}, "fade.regress_z"),
    ({"give_up": 0}, "fade.give_up"),
])
def test_a_bad_fade_config_is_refused_at_load(raw, key):
    with pytest.raises(ValueError, match=key):
        from_dict(TrainConfig, {"fade": raw})


def test_progress_carries_the_shaping_scale_on_protocol_18():
    """The sim reads PROGRESS as ProgressMsg {f32 progress, f32 shaping scale} (Bridge/Protocol.h), and refuses a
    learner of another protocol at HELLO, so a sim that would ignore the scale never runs with one that sends it."""
    assert p.PROTOCOL_VERSION == 18
    payload = p.encode_progress(0.25, 0.5)
    assert len(payload) == 8 and struct.unpack("<ff", payload) == (0.25, 0.5)
    assert struct.unpack("<ff", p.encode_progress(2.0, -1.0)) == (1.0, 0.0)   # clamped as the sim would
    assert struct.unpack("<ff", p.encode_progress(0.5)) == (0.5, 1.0)          # shaping in full by default
