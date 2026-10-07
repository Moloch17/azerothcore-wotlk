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


def test_a_gated_difficulty_ladder_never_steps_back_on_the_score_and_a_carried_hold_does_not_pin_it():
    """require_plateau off: the rungs are difficulty, and the outcome score falls at a harder rung by design. Read
    against the easier rung's score, every step up looked like a regression: M2 fell back from the doorway twice and was
    held at the hallway for good (2026-10-07). A gated ladder keeps its rung however the score drops, and falls a
    checkpoint carries from before hold nothing."""
    fade = _fade(gate_metric="found", gate_value=0.8, require_plateau=False)
    fade.see_gate({"found": 0.99})
    assert fade.observe(3.0, 0.1, 0) is not None and fade.rung == 1
    for env_steps in range(1, 8):                       # far below the step's score, the gate unmet
        fade.see_gate({"found": 0.35})
        assert fade.observe(-0.5, 0.1, env_steps) is None
    assert fade.rung == 1 and not fade.held and not fade.regresses

    stuck = _fade(gate_metric="found", gate_value=0.8, require_plateau=False)
    stuck.falls = {0: 2}                                # held at the first rung under the old rule
    assert not stuck.held and not stuck.settled
    stuck.see_gate({"found": 1.0})
    assert stuck.observe(3.0, 0.1, 0) is not None and stuck.rung == 1

    scored = _fade()                                    # a plateau ladder still regresses and holds
    assert scored.regresses


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


def test_without_require_plateau_a_gated_fade_steps_on_the_gate_alone_while_the_score_still_rises():
    """require_plateau off (the dungeon plan's rule): each rung steps at the first evaluation that meets the gate, even
    on a score still climbing every evaluation; under the gate it holds; with no gate the option is ignored."""
    fade = _fade(gate_metric="found", gate_value=0.8, require_plateau=False)
    steps = []
    for env_steps, (score, found) in enumerate([(1.0, 0.5), (2.0, 0.85), (3.0, 0.9), (4.0, 0.7), (5.0, 0.95)]):
        fade.see_gate({"found": found})
        steps.append(fade.observe(score, 0.01, env_steps) is not None)
    assert steps == [False, True, True, False, False]   # the last rung reached after two gated steps
    assert fade.scale == 0.0

    plateau = _fade(gate_metric="found", gate_value=0.8)  # the default still waits for the plateau
    moved = []
    for env_steps, score in enumerate([1.0, 2.0, 3.0]):
        plateau.see_gate({"found": 0.95})
        moved.append(plateau.observe(score, 0.01, env_steps) is not None)
    assert moved == [False, False, False]

    ungated = _fade(require_plateau=False)
    assert ungated.require_plateau   # no gate: plateau as before


def test_a_collapsed_rung_raises_the_alarm_once_and_never_steps_back():
    """A gate-stepped ladder's gate metric under its floor (0.1, or a quarter of the rung below's last reading) for
    three evaluations running raises one warning line and the `collapsed` flag (forge status); the rung stays."""
    fade = _fade(gate_metric="found", gate_value=0.8, require_plateau=False)
    fade.see_gate({"found": 0.95})
    fade.observe(3.0, 0.1, 0)
    assert fade.rung == 1 and fade.lower_gate == pytest.approx(0.95)
    alarms = []
    for env_steps, found in enumerate([0.3, 0.2, 0.1, 0.15, 0.6], start=1):   # floor 0.2375
        fade.see_gate({"found": found})
        fade.observe(-1.0, 0.1, env_steps)
        alarms.append((fade.alarm is not None, fade.collapsed))
    assert alarms == [(False, False), (False, False), (False, False), (True, True), (False, False)]
    assert fade.rung == 1
    again = _fade(gate_metric="found", gate_value=0.8, require_plateau=False)
    again.load_state_dict(fade.state_dict())
    assert again.rung_gates == fade.rung_gates and again.lower_gate == fade.lower_gate


def test_no_gate_stepped_stage_can_settle_below_its_top_rung():
    """Convergence needs the shaping ladder settled; a gate-stepped ladder is settled only at its last rung, whatever
    falls a checkpoint carries -- so no such stage (M2 on) converges at an easy rung."""
    from pathlib import Path
    configs = sorted((Path(__file__).resolve().parents[1] / "configs").glob("*.yaml"))
    gated = 0
    for path in configs:
        config = TrainConfig.load(path)
        if not (config.fade.enabled and config.fade.rungs and config.fade.gate_metric
                and not config.fade.require_plateau):
            continue
        gated += 1
        fade = ShapingFade(config)
        if len(fade.rungs) > 1:
            fade.falls = {0: 9}
            assert not fade.settled, path.name
        fade.rung = len(fade.rungs) - 1
        assert fade.settled, path.name
    assert gated >= 10


def _gated_fade(**fade) -> ShapingFade:
    return _fade(gate_metric="found", gate_value=0.99, require_plateau=False, stall_evals=6,
                 stall_env_steps=20_000_000, **fade)


def _read(fade: ShapingFade, found: float, env_steps: int, episodes: int = 64) -> bool:
    """One evaluation's gate reading; whether it raised the stall line."""
    fade.see_gate({"found": found, "episodes": episodes})
    fade.observe(-1.0, 0.1, env_steps, ladders_settled=False)   # settled off: the rung holds
    return fade.stall_alarm is not None


def test_a_flat_rung_raises_the_stall_warning_once_after_its_evaluations_and_its_steps():
    """Flat at 0.35 within its standard error: no warning before K evaluations AND N env steps, one line then, and the
    ladder takes no action."""
    fade = _gated_fade()
    raised = [_read(fade, 0.35 + 0.001 * (k % 2), 4_000_000 * (k + 1)) for k in range(12)]
    # The best (0.35) is set at the first read, 4M steps; the sixth evaluation after it is at 28M (24M later).
    assert raised == [False] * 6 + [True] + [False] * 5
    assert fade.stalled and fade.rung == 0 and fade.report()["stalled"]

    steps_short = _gated_fade()   # K evaluations in a hurry: the steps have not passed
    assert not any(_read(steps_short, 0.35, 1_000_000 * (k + 1)) for k in range(12))
    assert not steps_short.stalled
    evals_short = _gated_fade()   # N steps with too few evaluations
    assert not any(_read(evals_short, 0.35, 10_000_000 * (k + 1)) for k in range(5))


def test_a_rising_rung_never_stalls_and_a_stall_clears_when_the_best_improves():
    rising = _gated_fade()
    assert not any(_read(rising, 0.3 + 0.05 * k, 5_000_000 * (k + 1)) for k in range(12))
    assert not rising.stalled

    fade = _gated_fade()
    for k in range(8):
        _read(fade, 0.35, 5_000_000 * (k + 1))
    assert fade.stalled
    # 64 episodes at 0.35: stderr 0.060, so +0.05 does not clear it; +0.07 does.
    assert not _read(fade, 0.40, 45_000_000) and fade.stalled
    assert not _read(fade, 0.43, 50_000_000) and not fade.stalled and fade.gate_best == pytest.approx(0.43)
    again = [_read(fade, 0.43, 50_000_000 + 5_000_000 * (k + 1)) for k in range(8)]
    assert again.count(True) == 1 and fade.stalled     # a new episode of it warns once more


def test_the_stall_survives_a_resume_and_a_step_starts_the_rung_over():
    fade = _gated_fade()
    for k in range(8):
        _read(fade, 0.35, 5_000_000 * (k + 1))
    resumed = _gated_fade()
    resumed.load_state_dict(fade.state_dict())
    assert resumed.stalled and resumed.gate_best == fade.gate_best and resumed.stall_alarm is None
    assert not _read(resumed, 0.35, 45_000_000) and resumed.stalled     # still stalled, not warned again
    # An old checkpoint without the keys loads as a fresh watch.
    old = fade.state_dict()
    for key in ("gate_best", "gate_best_steps", "evals_since_gate_best", "stalled"):
        old.pop(key)
    legacy = _gated_fade()
    legacy.load_state_dict(old)
    assert legacy.gate_best is None and not legacy.stalled

    stepped = _gated_fade()
    for k in range(8):
        _read(stepped, 0.35, 5_000_000 * (k + 1))
    stepped.see_gate({"found": 1.0, "episodes": 64})
    stepped.observe(-1.0, 0.1, 45_000_000)       # gate met: the rung steps
    assert stepped.rung == 1 and not stepped.stalled and stepped.gate_best is None


def test_the_stall_warning_ignores_plateau_stepped_ladders_and_the_last_rung():
    plateau = _fade(gate_metric="found", gate_value=0.99, stall_evals=1, stall_env_steps=0)   # require_plateau on
    assert not any(_read(plateau, 0.35, 5_000_000 * (k + 1)) for k in range(10))
    last = _gated_fade()
    last.rung = len(last.rungs) - 1
    assert not any(_read(last, 0.35, 5_000_000 * (k + 1)) for k in range(10))


def test_the_gate_standard_error_is_the_summarys_then_binomial_then_fixed():
    from animus.stage import gate_stderr
    assert gate_stderr({"found_stderr": 0.03, "episodes": 64}, "found", 0.5) == pytest.approx(0.03)
    assert gate_stderr({"episodes": 100}, "found", 0.35) == pytest.approx((0.35 * 0.65 / 100) ** 0.5)
    assert gate_stderr({}, "found", 0.35) == pytest.approx(0.02)
    assert gate_stderr({"episodes": 64}, "arrive_seconds", 12.0) == pytest.approx(0.02)   # not a share


def test_the_stall_settings_are_validated_and_set_per_stage():
    assert FadeConfig().stall_evals == 6 and FadeConfig().stall_env_steps == 20_000_000
    assert FadeConfig(stall_evals=3, stall_env_steps=0).stall_evals == 3
    with pytest.raises(ValueError, match="stall_evals"):
        FadeConfig(stall_evals=0)
    with pytest.raises(ValueError, match="stall_env_steps"):
        FadeConfig(stall_env_steps=-1)
    config = TrainConfig()
    config.fade = FadeConfig(enabled=True, rungs=(1.0, 0.0), gate_metric="found", gate_value=0.9, require_plateau=False,
                             stall_evals=2, stall_env_steps=1)
    assert ShapingFade(config).stall_evals == 2
