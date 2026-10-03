"""peak-play W0: the evaluation is judged on the episode's Outcome and Cost terms, and the reward audit reads the
sim's own categories."""

import numpy as np
import pytest

from animus.config import TrainConfig
from animus.evaluation import EvalResult, standard_error
from animus.rewards import OUTCOME_TERMS, audit, outcome_terms


def test_the_score_is_the_outcome_column_not_the_return():
    """score_outcome is the episode's Outcome and Cost terms before any rung's tier, so shaping turned down or a
    ladder stepping does not move the yardstick. The return stays in the summary beside it."""
    infos = np.array([[2.0, 0.0], [4.0, 0.0], [6.0, 0.0]], dtype=np.float32)
    result = EvalResult("learner", np.array([10.0, 20.0, 30.0]), infos, ("score_outcome", "level"))
    assert result.score == pytest.approx(4.0)
    summary = result.summary(())
    assert summary["score"] == pytest.approx(4.0)
    assert summary["return"] == pytest.approx(20.0)
    assert result.stderr == pytest.approx(standard_error(np.array([2.0, 4.0, 6.0])))


def test_the_score_falls_back_to_the_return():
    """A stage.json from before the column, or eval.score: return, scores the whole return as before."""
    old = EvalResult("learner", np.array([1.0, 3.0]), np.zeros((2, 1), np.float32), ("level",))
    assert old.score == pytest.approx(2.0)
    chosen = EvalResult("learner", np.array([1.0, 3.0]), np.array([[5.0], [7.0]], np.float32), ("score_outcome",),
                        score_column="")
    assert chosen.score == pytest.approx(2.0)


def test_per_layout_rows_are_scored_on_the_column():
    infos = np.array([[1.0], [3.0], [10.0]], dtype=np.float32)
    result = EvalResult("learner", np.array([0.0, 0.0, 0.0]), infos, ("score_outcome",),
                        layouts=("mage_dps", "mage_dps", "warrior_dps"))
    layouts = result.summary(())["layouts"]
    assert layouts["mage_dps"]["score"] == pytest.approx(2.0)
    assert layouts["warrior_dps"]["score"] == pytest.approx(10.0)


def test_merged_shares_keep_the_score_column():
    one = EvalResult("learner", np.array([1.0]), np.array([[5.0]], np.float32), ("score_outcome",), seeds=(0,))
    two = EvalResult("learner", np.array([3.0]), np.array([[7.0]], np.float32), ("score_outcome",), seeds=(1,))
    assert EvalResult.merged([one, two]).score == pytest.approx(6.0)
    returns = EvalResult("learner", np.array([1.0]), np.array([[5.0]], np.float32), ("score_outcome",), seeds=(0,),
                         score_column="")
    assert EvalResult.merged([returns, returns]).score == pytest.approx(1.0)


def test_eval_score_option():
    config = TrainConfig()
    assert config.eval.score_column() == "score_outcome"
    config.eval.score = "return"
    assert config.eval.score_column() == ""
    config.eval.score = "wins"
    with pytest.raises(ValueError):
        config.eval.score_column()


def test_outcome_terms_come_from_the_sim():
    """stage.json's reward_terms (RewardTermCategory) say what is outcome; the hand list is only the fallback for an
    older stage.json."""
    stage = {"reward_terms": {"kill": "outcome", "death": "cost", "pull_clean": "shaping", "approach": "shaping"}}
    assert set(outcome_terms(stage)) == {"kill", "death"}
    assert outcome_terms({}) == OUTCOME_TERMS
    assert outcome_terms(None) == OUTCOME_TERMS
    # The drill's clean pull is shaping now that it has its own term: judged against the kill, not as one.
    finding = audit({"kill": 2.0, "pull_clean": 1.5}, outcome_terms(stage))
    assert finding is not None and finding[0] == "pull_clean"
