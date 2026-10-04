"""The cost ladder: the noise prices climb to full price as the outcome plateaus at each rung, step back down when the
outcome is given up for them, and hold convergence and the learning-rate anneal until full price has been played."""

import pytest

from animus.config import CostLadderConfig, FadeConfig, TrainConfig, from_dict
from animus.stage import ConvergenceController, CostLadder, restore_evaluation_state


def _config(**costs) -> TrainConfig:
    config = TrainConfig()
    config.costs = CostLadderConfig(**{"enabled": True, "rungs": (0.25, 0.5, 1.0), "window": 2, "give_up": 2,
                                       **costs})
    return config


def _play(ladder: CostLadder, scores, start=0) -> list[float]:
    scales = []
    for index, score in enumerate(scores):
        ladder.observe(score, 0.1, (start + index + 1) * 10)
        scales.append(ladder.scale)
    return scales


def test_the_ladder_climbs_on_a_plateau_and_is_ready_after_a_window_at_full_price():
    ladder = CostLadder(_config())
    assert ladder.scale == 0.25 and not ladder.settled and not ladder.ready
    assert _play(ladder, [5] * 6) == pytest.approx([0.25, 0.25, 0.5, 0.5, 0.5, 1.0])
    assert ladder.settled and not ladder.ready                       # at full price, not yet played there
    _play(ladder, [5, 5], start=6)
    assert ladder.scale == 1.0 and ladder.ready
    # A climbing score is no plateau: it stays cheap while the policy is still finding the outcome.
    climbing = CostLadder(_config())
    assert _play(climbing, [1, 2, 3, 4, 5, 6]) == [0.25] * 6


def test_giving_up_the_outcome_for_the_price_steps_back_down():
    ladder = CostLadder(_config())
    _play(ladder, [5, 5, 5])
    assert ladder.scale == 0.5
    message = None
    for steps, score in ((40, 3.0), (50, 3.0)):
        message = ladder.observe(score, 0.1, steps) or message
    assert ladder.scale == 0.25 and "cost ladder moves back" in message and "x0.5 -> x0.25" in message


def test_off_is_full_price_and_never_holds_anything():
    ladder = CostLadder(TrainConfig())
    assert _play(ladder, [5] * 4 + [0] * 3) == [1.0] * 7 and ladder.settled and ladder.ready


def test_the_anneal_convergence_and_the_shaping_fade_wait_for_full_price():
    config = _config()
    config.fade = FadeConfig(enabled=True, rungs=(1.0, 0.0), window=2)
    config.convergence.patience = 3   # the anneal starts at the overall score's plateau (0 would never anneal)
    config.eval.every_env_steps = 10  # and only an evaluating run has one
    controller = ConvergenceController(config, ["mage_dps"])
    summary = {"score": 5.0, "stderr": 0.1, "layouts": {"mage_dps": {"score": 5.0, "stderr": 0.1}}}
    steps = 0
    while not controller.costs.ready:
        steps += 10
        controller.observe(summary, steps)
        assert controller.plateau_env_steps is None, steps         # no anneal while noise is cheap
        assert controller.fade.scale == 1.0, steps                  # nor a shaping step
        assert not controller.layouts["mage_dps"].converged, steps  # nor a class converged
        assert steps < 1000
    assert controller.costs.scale == 1.0
    for _ in range(12):
        steps += 10
        controller.observe(summary, steps)
    assert controller.plateau_env_steps is not None and controller.fade.scale == 0.0


def test_state_round_trips_through_a_resume():
    config = _config()
    controller = ConvergenceController(config, ["mage_dps"])
    summary = {"score": 5.0, "stderr": 0.1, "layouts": {}}
    for steps in (10, 20, 30):
        controller.observe(summary, steps)
    assert controller.costs.scale == 0.5
    saved = {"convergence": controller.tracker.state_dict(), "controller": controller.state_dict(),
             "score_kind": "score_outcome"}
    again = ConvergenceController(config, ["mage_dps"])
    restore_evaluation_state(again.tracker, again, saved, "score_outcome")
    assert again.costs.state_dict() == controller.costs.state_dict() and again.costs.scale == 0.5
    # A checkpoint from before the ladder starts at its first rung.
    old = {**saved, "controller": {k: v for k, v in saved["controller"].items() if k != "costs"}}
    fresh = ConvergenceController(config, ["mage_dps"])
    restore_evaluation_state(fresh.tracker, fresh, old, "score_outcome")
    assert fresh.costs.scale == 0.25


def test_free_until_the_gate_then_in_and_back_out():
    """stage1_move's path: x0 until the evaluation arrives often enough (a plateau short of it is no step), in to full
    price and out again on plateaus; a habit that does not stick at the lower price is a fall that moves it back."""
    ladder = CostLadder(_config(rungs=(0.0, 0.5, 1.0, 0.5, 0.0), gate_metric="arrived", gate_value=0.8))
    scales = []
    for index, arrived in enumerate([0.06, 0.06, 0.06, 0.06, 0.5, 0.79]):   # flat score, arriving short of the gate
        ladder.see_gate({"arrived": arrived})
        ladder.observe(5, 0.1, (index + 1) * 10)
        scales.append(ladder.scale)
    assert scales == [0.0] * 6
    ladder.see_gate({"arrived": 0.85})
    message = ladder.observe(5, 0.1, 70)
    assert ladder.scale == 0.5 and "moves on" in message and "arrived 0.85 reached 0.8" in message
    ladder.see_gate({})                                                     # the gate is read off the first rung only
    assert _play(ladder, [5] * 6, start=7) == pytest.approx([0.5, 0.5, 1.0, 1.0, 1.0, 0.5])
    # Fading out, the habits slip: the full-price score falls and the ladder moves back to x1.
    for steps in (140, 150):
        ladder.observe(4.0, 0.1, steps)
    assert ladder.scale == 1.0 and ladder.falls == {2: 1}
    # Without a gate the first rung steps on a plateau, as any other.
    plain = CostLadder(_config(rungs=(0.0, 1.0)))
    assert _play(plain, [5, 5, 5]) == pytest.approx([0.0, 0.0, 1.0])


def test_a_ladder_cut_short_under_a_resume_lands_on_full_price_as_a_fresh_step():
    """stage1_move's fade-out was cut while the run was on x0: the resumed ladder is at x1 with its wait, plateau and
    step score started over, and the classes' convergence and the learning-rate anneal start over with it."""
    long = _config(rungs=(0.0, 1.0, 0.0))
    long.convergence.patience = 3
    long.eval.every_env_steps = 10
    controller = ConvergenceController(long, ["mage_dps"])
    controller.costs.rung = 2
    controller.costs.evals_at_rung = 9
    controller.plateau_env_steps = 100
    controller.layouts["mage_dps"].converged = True
    saved = {"convergence": controller.tracker.state_dict(), "controller": controller.state_dict(),
             "score_kind": "score_outcome"}

    short = _config(rungs=(0.0, 1.0))
    short.convergence.patience = 3
    short.eval.every_env_steps = 10
    again = ConvergenceController(short, ["mage_dps"])
    restore_evaluation_state(again.tracker, again, saved, "score_outcome")
    assert again.costs.scale == 1.0 and again.costs.reshaped and again.costs.evals_at_rung == 0
    assert again.costs.settled and not again.costs.ready
    assert again.plateau_env_steps is None and not again.layouts["mage_dps"].converged

    # A resume within the ladder's length is left as it was.
    same = ConvergenceController(long, ["mage_dps"])
    restore_evaluation_state(same.tracker, same, saved, "score_outcome")
    assert not same.costs.reshaped and same.costs.evals_at_rung == 9 and same.plateau_env_steps == 100


@pytest.mark.parametrize("raw", [
    {"rungs": [0.5, 0.5, 1.0]},      # a rung like the one before is no step
    {"rungs": [0.0, 1.5]},           # above full price
    {"rungs": [-0.25, 1.0]},
    {"rungs": []},
    {"window": 0},
    {"regress_z": 0.0},
    {"give_up": 0},
])
def test_a_bad_cost_ladder_is_refused_at_load(raw):
    with pytest.raises(ValueError, match="costs\\."):
        from_dict(TrainConfig, {"costs": raw})
