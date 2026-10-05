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


def test_a_regression_steps_back_up_on_the_window_mean_waits_and_is_held_after_give_up():
    """Regression is read off the mean of the rung's last `window` scores, and only once it has that many: a single
    noisy evaluation (a ~2% false alarm per evaluation at regress_z 2) must not undo a step, nor the dip right after
    one, but a sustained drop must."""
    fade = _fade(window=4)
    assert _play(fade, [5] * 4) == [1] * 4
    assert fade.observe(5, 0.1, 50) and fade.scale == 0.5           # plateau: down to 0.5, step score 5 +/- 0.1
    # One evaluation 3 standard errors low among good ones -- the old single-score test stepped back on it.
    for steps, score in ((60, 5.0), (70, 4.7), (80, 5.0), (90, 5.0), (100, 5.0)):
        assert fade.observe(score, 0.1, steps) is None and fade.scale == 0.5
    # A sustained drop: nothing fires within the first window after the step; it does once the window's mean is low.
    fresh = _fade(window=4)
    _play(fresh, [5] * 5)
    assert fresh.scale == 0.5
    for steps in (60, 70, 80):
        assert fresh.observe(4.5, 0.1, steps) is None and fresh.scale == 0.5
    message = fresh.observe(4.5, 0.1, 90)
    assert fresh.scale == 1.0 and "steps back up" in message and "x0.5 -> x1" in message
    assert "over the last 4 evaluations" in message
    assert fresh.falls == {0: 1} and not fresh.held and not fresh.settled
    # The wait: a full window at the rung before it may step again, however flat.
    assert _play(fresh, [5] * 4) == [1] * 4
    assert fresh.observe(5, 0.1, 200) and fresh.scale == 0.5        # tried again
    _play(fresh, [4.5] * 4)                                         # and fell again: held at full shaping
    assert fresh.scale == 1.0 and fresh.held and fresh.settled
    assert _play(fresh, [5] * 8) == [1] * 8                          # held means held

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
    ({"moving_classes": -1}, "fade.moving_classes"),
])
def test_a_bad_fade_config_is_refused_at_load(raw, key):
    with pytest.raises(ValueError, match=key):
        from_dict(TrainConfig, {"fade": raw})


def test_progress_carries_the_shaping_and_cost_scales_on_protocol_19():
    """The sim reads PROGRESS as ProgressMsg {f32 progress, f32 shaping scale, f32 cost scale} (Bridge/Protocol.h),
    and refuses a learner of another protocol at HELLO, so a sim that would ignore a scale never runs with one that
    sends it."""
    assert p.PROTOCOL_VERSION >= 19
    payload = p.encode_progress(0.25, 0.5, 0.25)
    assert len(payload) == 12 and struct.unpack("<fff", payload) == (0.25, 0.5, 0.25)
    assert struct.unpack("<fff", p.encode_progress(2.0, -1.0, 3.0)) == (1.0, 0.0, 1.0)   # clamped as the sim would
    assert struct.unpack("<fff", p.encode_progress(0.5)) == (0.5, 1.0, 1.0)               # both in full by default


@pytest.mark.parametrize("moving_classes, restless, settled", [
    (1, 1, True),       # one class still climbing its ladder does not hold the fade
    (1, 2, False),      # two do
    (0, 1, False),      # 0: every class still, as before
])
def test_the_fade_waits_for_the_ladders_but_not_for_every_last_class(moving_classes, restless, settled):
    config = TrainConfig()
    config.fade = FadeConfig(enabled=True, moving_classes=moving_classes)
    names = ["mage_dps", "warrior_dps", "priest_heal"]
    controller = ConvergenceController(config, names)
    for index, name in enumerate(names):
        state = controller.layouts[name]
        state.played = True
        # A restless class's rung moved by a whole rung over the window; a settled one by a tenth.
        state.rung = [1.0, 2.0, 3.0, 4.0] if index < restless else [3.0, 3.1, 3.0, 3.1]
    assert controller.ladders_settled() == settled


def test_a_gated_fade_waits_for_the_stage_measure_then_steps_on_plateaus():
    """stage3_rotation 2026-10-05: faded on plateaus alone, the shaping ladder reached x0 with output at ~5 a dummy and
    the policy stopped casting. With a gate, a plateau short of the measure is no step."""
    fade = _fade(gate_metric="dummy_output", gate_value=4.0)
    scales = []
    for index, output in enumerate([1.0, 2.0, 3.0, 3.5, 3.9, 3.9]):         # flat score, output short of the gate
        fade.see_gate({"dummy_output": output})
        fade.observe(5, 0.1, (index + 1) * 10)
        scales.append(fade.scale)
    assert scales == [1.0] * 6
    fade.see_gate({"dummy_output": 4.2})
    message = fade.observe(5, 0.1, 70)
    assert fade.scale == 0.5 and "dummy_output 4.2 (gate 4)" in message
    # A summary without the column holds it too (another stage's evaluation, a baseline).
    held = _fade(gate_metric="dummy_output", gate_value=4.0)
    held.see_gate({})
    assert _play(held, [5] * 6) == [1.0] * 6


def test_stage3_fades_on_its_output_and_stage4_keeps_the_plain_plateau():
    from pathlib import Path
    configs = Path(__file__).resolve().parents[1] / "configs"
    config = TrainConfig.load(configs / "stage3_rotation.yaml")
    assert config.fade.enabled and config.fade.gate_metric == "dummy_output" and config.fade.gate_value == 4.0
    assert config.fade.rungs == (1.0, 0.5, 0.25, 0.0)
    stage4 = TrainConfig.load(configs / "stage4_duel.yaml")
    assert stage4.fade.enabled and stage4.fade.gate_metric == ""
