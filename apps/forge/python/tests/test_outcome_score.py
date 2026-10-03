"""peak-play W0: the evaluation is judged on the episode's Outcome and Cost terms, and the reward audit reads the
sim's own categories."""

import numpy as np
import pytest

from animus.config import TrainConfig
from animus.evaluation import EvalResult, standard_error
from animus.rewards import OUTCOME_TERMS, audit, outcome_terms
from animus.stage import ConvergenceController, restore_evaluation_state
from animus.train import baseline_cache_key


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


def _scored_checkpoint(score_kind: str | None) -> dict:
    """A checkpoint's evaluation state after three evaluations on the return (a pre-W0 run when score_kind is None)."""
    config = TrainConfig()
    config.convergence.patience = 2
    controller = ConvergenceController(config, ["mage_dps"])
    for steps, score in ((10, 50.0), (20, 60.0), (30, 61.0)):
        controller.observe({"score": score, "stderr": 1.0, "layouts": {"mage_dps": {"score": score, "stderr": 1.0}}},
                           steps)
        controller.observe_update({"mage_dps": {"approx_kl": 0.002, "entropy": 1.0, "allowed_actions": 4}}, 1.0)
    checkpoint = {"convergence": controller.tracker.state_dict(), "controller": controller.state_dict()}
    if score_kind is not None:
        checkpoint["score_kind"] = score_kind
    return checkpoint


def test_a_pre_w0_checkpoint_resumed_on_the_outcome_score_starts_its_scores_over():
    """Its best and history were measured on the return: an outcome score compared with them would never set a new
    best.pt, and a trend read across the two would be nonsense. The scale-free signals (KL here) are kept."""
    config = TrainConfig()
    controller = ConvergenceController(config, ["mage_dps"])
    tracker = controller.tracker
    reason = restore_evaluation_state(tracker, controller, _scored_checkpoint(None), "score_outcome")
    assert reason is not None and "the return" in reason
    assert tracker.best is None and tracker.history == [] and tracker.evals_since_best == 0
    assert tracker.best_env_steps == 0
    state = controller.layouts["mage_dps"]
    assert state.scores == [] and state.tracker.best is None and not state.converged
    assert controller.best_summary is None
    assert state.kl  # the KL signal means the same on either score
    # The first outcome evaluation is the new best.
    assert controller.observe({"score": 3.0, "stderr": 0.1, "layouts": {}}, 40)


def test_a_checkpoint_of_the_same_kind_resumes_as_it_was():
    config = TrainConfig()
    controller = ConvergenceController(config, ["mage_dps"])
    assert restore_evaluation_state(controller.tracker, controller, _scored_checkpoint("score_outcome"),
                                    "score_outcome") is None
    assert controller.tracker.best == pytest.approx(60.0) and len(controller.tracker.history) == 3
    # A pre-W0 checkpoint resumed by a run still scoring the return keeps everything too.
    again = ConvergenceController(config, ["mage_dps"])
    assert restore_evaluation_state(again.tracker, again, _scored_checkpoint(None), "") is None
    assert len(again.tracker.history) == 3


def test_a_changed_score_kind_misses_the_baseline_cache():
    """A baseline summarised on the return must be scored again for a run that scores outcomes: its per-class gaps
    weight the training draw."""
    old = baseline_cache_key("fight", 1000, 64, "", ("solo",), {"Kill": 5.0}, "")
    new = baseline_cache_key("fight", 1000, 64, "", ("solo",), {"Kill": 5.0}, "score_outcome")
    assert old != new
    assert new == baseline_cache_key("fight", 1000, 64, "", ["solo"], {"Kill": 5.0}, "score_outcome")


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
