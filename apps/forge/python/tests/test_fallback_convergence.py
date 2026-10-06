"""A marker stage whose narrow legs (above, below, upstairs, across water, a lakebed) fall back to ordinary markers
cannot converge on them (overseer, 2026-10-05): M3 and M4 converge on arrived_narrow -- arrivals on real narrow legs
over those legs, fallbacks excluded -- and a class is missing "fallbacks" while its top-rung evaluation's
fallback_share is over the ceiling (convergence.fallback_ceiling, else the sim's Markers.FallbackCeiling). A
convergence signal, not a gate: the stage ends on its signals or its budget."""

import numpy as np
import pytest

from animus.evaluation import EvalResult
from animus.stage import ADVANCE, CONTINUE, ConvergenceController
from test_stage import CLASSES, _config
from test_top_rung_convergence import _evals

pytest.importorskip("torch")


def _summary(arrived_narrow: float, fallback_share: float) -> dict:
    def row():
        return {"score": 5.0, "stderr": 0.1, "episodes": 64, "arrived": 0.95, "arrived_narrow": arrived_narrow,
                "fallback_share": fallback_share}
    out = {"score": 5.0, "stderr": 0.1, "layouts": {name: row() for name in CLASSES}}
    out["top_rung"] = {"score": 5.0, "episodes": 64, "layouts": {name: row() for name in CLASSES}}
    return out


def _controller(**overrides):
    return ConvergenceController(_config(**{"convergence.measure": "arrived_narrow", **overrides}), CLASSES,
                                 sim_fallback_ceiling=0.2)


def test_half_the_legs_falling_back_never_converges():
    controller = _controller()
    outcomes = _evals(controller, [_summary(0.9, 0.5)] * 8, top_share=1.0)
    assert all(outcome.action == CONTINUE for outcome in outcomes)
    assert controller.report()["warrior_dps"]["missing"] == ["fallbacks"]


def test_under_the_ceiling_it_converges_on_arrived_narrow():
    controller = _controller()
    outcomes = _evals(controller, [_summary(0.8, 0.1)] * 4, top_share=1.0)
    assert outcomes[-1].action == ADVANCE
    assert controller.layouts["mage_dps"].scores[-1] == pytest.approx(0.8)


def test_the_configs_own_ceiling_wins_and_zero_turns_it_off():
    strict = _controller(**{"convergence.fallback_ceiling": 0.05})
    assert _evals(strict, [_summary(0.8, 0.1)] * 6, top_share=1.0)[-1].action == CONTINUE
    off = _controller(**{"convergence.fallback_ceiling": 0.0})
    assert _evals(off, [_summary(0.8, 0.5)] * 4, top_share=1.0)[-1].action == ADVANCE


def test_the_summary_counts_narrow_legs_by_sums():
    """arrived_narrow and fallback_share are ratios of sums: an episode with no narrow leg weighs nothing."""
    names = ("narrow_legs", "narrow_arrived", "fallback_legs", "at_top_rung")
    infos = np.array([[4, 3, 0, 1], [0, 0, 2, 1], [2, 1, 2, 1]], dtype=np.float32)
    result = EvalResult(policy="learner", returns=np.zeros(len(infos)), infos=infos, info_names=names,
                        layouts=("warrior_dps",) * 3, score_column="")
    summary = result.summary(())
    assert summary["arrived_narrow"] == pytest.approx(4 / 6)
    assert summary["fallback_share"] == pytest.approx(4 / 10)
    assert summary["top_rung"]["arrived_narrow"] == pytest.approx(4 / 6)
