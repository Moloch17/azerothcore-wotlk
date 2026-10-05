"""A ladder stage converges only at the top of its ladder (convergence.top_rung; user, 2026-10-05, movement-curriculum
M1): its classes are judged on their evaluation episodes at the top rung, on the stage's own measure, and none
converges until its training episodes have been at the top rung for the whole window -- every laddered stage, not
just M1. A stage with no ladder converges as before."""

import math

import numpy as np
import pytest

from animus.evaluation import EvalResult
from animus.stage import ADVANCE, CONTINUE, ConvergenceController
from test_stage import CLASSES, _config

pytest.importorskip("torch")


def _ladder_summary(top: dict[str, float] | None, lower: dict[str, float], measure: str = "arrived") -> dict:
    """An evaluation whose episodes are spread over the rungs: `lower` the classes' share on the lower rungs, `top`
    at the top (None: no evaluation episode reached it)."""
    def row(value):
        return {"score": value * 10.0, "stderr": 0.1, "episodes": 64, measure: value}
    out = {"score": 1.0, "stderr": 0.1, "layouts": {name: row(value) for name, value in lower.items()}}
    out["top_rung"] = {"score": None, "episodes": 0, "layouts": {} if top is None else {
        name: row(value) for name, value in top.items()}}
    return out


def _evals(controller, summaries, top_share: float | None, kl: float = 0.001):
    outcomes = []
    for index, summary in enumerate(summaries):
        steps = 100 * (index + 1)
        controller.observe_update({name: {"approx_kl": kl, "entropy": 0.5 * math.log(10.0), "allowed_actions": 10.0}
                                   for name in CLASSES}, 1.0)
        controller.observe_training_episodes({name: 3.0 for name in CLASSES},
                                             None if top_share is None else {name: top_share for name in CLASSES})
        controller.observe(summary, steps)
        outcomes.append(controller.after_eval(steps))
    return outcomes


def test_a_class_settled_on_a_lower_rung_does_not_converge():
    controller = ConvergenceController(_config(**{"convergence.measure": "arrived"}), CLASSES)
    flat = _ladder_summary(None, {"warrior_dps": 0.9, "mage_dps": 0.9})
    outcomes = _evals(controller, [flat] * 6, top_share=0.0)
    assert all(outcome.action == CONTINUE for outcome in outcomes)
    assert "top_rung" in controller.report()["warrior_dps"]["missing"]
    assert controller.layouts["warrior_dps"].scores == []      # no score from another rung


def test_training_off_the_top_blocks_even_when_the_evaluation_reaches_it():
    controller = ConvergenceController(_config(**{"convergence.measure": "arrived"}), CLASSES)
    flat = _ladder_summary({"warrior_dps": 0.8, "mage_dps": 0.8}, {"warrior_dps": 0.95, "mage_dps": 0.95})
    outcomes = _evals(controller, [flat] * 5, top_share=0.5)
    assert all(outcome.action == CONTINUE for outcome in outcomes)
    assert controller.report()["mage_dps"]["missing"] == ["top_rung"]


def test_at_the_top_the_stage_converges_on_its_own_measure():
    controller = ConvergenceController(_config(**{"convergence.measure": "arrived"}), CLASSES)
    flat = _ladder_summary({"warrior_dps": 0.82, "mage_dps": 0.7}, {"warrior_dps": 0.97, "mage_dps": 0.95})
    outcomes = _evals(controller, [flat] * 4, top_share=1.0)
    assert outcomes[-1].action == ADVANCE and outcomes[-1].reason == "converged"
    # The plateau read the top rung's `arrived`, not the score of the easier rungs.
    assert controller.layouts["warrior_dps"].scores[-1] == pytest.approx(0.82)
    assert controller.layouts["mage_dps"].scores[-1] == pytest.approx(0.7)


def test_a_climbing_top_rung_blocks_while_the_lower_rungs_are_flat():
    controller = ConvergenceController(_config(**{"convergence.measure": "arrived",
                                                  "convergence.min_improvement_abs": 0.05}), CLASSES)
    summaries = [_ladder_summary({"warrior_dps": 0.3 + 0.15 * i, "mage_dps": 0.6}, {"warrior_dps": 0.95,
                                                                                    "mage_dps": 0.95})
                 for i in range(5)]
    outcomes = _evals(controller, summaries, top_share=1.0)
    assert all(outcome.action == CONTINUE for outcome in outcomes)
    assert "score" in controller.report()["warrior_dps"]["missing"]


def test_a_stage_without_a_ladder_is_unaffected():
    controller = ConvergenceController(_config(), CLASSES)
    flat = {"score": 4.0, "stderr": 0.0, "layouts": {name: {"score": 4.0, "stderr": 0.0, "episodes": 64}
                                                      for name in CLASSES}}
    outcomes = _evals(controller, [flat] * 4, top_share=None)
    assert outcomes[-1].action == ADVANCE


def test_top_rung_can_be_switched_off():
    controller = ConvergenceController(_config(**{"convergence.top_rung": False}), CLASSES)
    flat = _ladder_summary(None, {"warrior_dps": 0.9, "mage_dps": 0.9})
    outcomes = _evals(controller, [flat] * 4, top_share=0.0)
    assert outcomes[-1].action == ADVANCE


def test_the_state_survives_a_resume():
    config = _config(**{"convergence.measure": "arrived"})
    controller = ConvergenceController(config, CLASSES)
    flat = _ladder_summary({"warrior_dps": 0.8, "mage_dps": 0.8}, {"warrior_dps": 0.9, "mage_dps": 0.9})
    _evals(controller, [flat] * 2, top_share=1.0)
    restored = ConvergenceController(config, CLASSES)
    restored.load_state_dict(controller.state_dict())
    state = restored.layouts["warrior_dps"]
    assert state.ladder and state.top_scored and state.top == [1.0, 1.0]
    assert _evals(restored, [flat] * 2, top_share=1.0)[-1].action == ADVANCE


def test_the_evaluation_summary_groups_the_top_rung():
    """EvalResult.summary's `top_rung`: the episodes at_top_rung marks, per layout."""
    names = ("arrived", "difficulty", "at_top_rung")
    infos = np.array([[1.0, 0, 0], [0.0, 0, 0], [1.0, 3, 1], [0.0, 3, 1], [1.0, 3, 1]], dtype=np.float32)
    result = EvalResult(policy="learner", returns=np.zeros(len(infos)), infos=infos, info_names=names,
                        layouts=("warrior_dps", "mage_dps", "warrior_dps", "mage_dps", "warrior_dps"), score_column="")
    top = result.summary(("arrived",))["top_rung"]
    assert top["episodes"] == 3 and top["arrived"] == pytest.approx(2 / 3)
    assert top["layouts"]["warrior_dps"]["arrived"] == pytest.approx(1.0)
    assert top["layouts"]["mage_dps"]["arrived"] == pytest.approx(0.0)
    # Without at_top_rung, the highest of several difficulty tiers.
    plain = EvalResult(policy="learner", returns=np.zeros(len(infos)), infos=infos[:, :2],
                       info_names=names[:2], layouts=result.layouts, score_column="")
    assert plain.summary(("arrived",))["top_rung"]["episodes"] == 3
