"""A gate-stepped ladder re-baselines the stage-level convergence at every rung (M2, 2026-10-07).

Its rungs are harder by design, so the score falls at each; read against the best ever seen (the easy rung's) every
evaluation looked like no progress: the learning rate annealed while the ladder was still climbing, and a class could be
called converged as soon as the top rung was reached."""

import json
import math

import numpy as np
import pytest

from animus.config import FadeConfig, TrainConfig
from animus.episode_means import means, undefined
from animus.progress import write_progress
from animus.stage import ADVANCE, ConvergenceController, restore_evaluation_state

CLASSES = ("warrior_dps", "mage_dps")


def _config(**fade) -> TrainConfig:
    config = TrainConfig()
    config.total_env_steps = 5000
    config.eval.every_env_steps = 100
    config.convergence.patience = 2
    config.convergence.window = 3
    config.convergence.kl = 0.003
    config.convergence.min_improvement_abs = 0.5
    config.convergence.min_improvement = 0.0
    config.entropy_floor.fraction = 0.0
    config.mappo.lr_final_fraction = 0.1
    config.fade = FadeConfig(**{"enabled": True, "rungs": (1.0, 0.66, 0.33, 0.0), "window": 2, "give_up": 2,
                                "gate_metric": "found", "gate_value": 0.8, "require_plateau": False, **fade})
    return config


def _eval(controller: ConvergenceController, index: int, score: float, found: float):
    """One update and one evaluation, 100 steps apart; the outcome after it."""
    steps = 100 * (index + 1)
    controller.observe_update({name: {"approx_kl": 0.001, "entropy": 0.5 * math.log(10.0), "allowed_actions": 10.0}
                               for name in CLASSES}, controller.lr_scale(steps))
    controller.observe_training_episodes({name: 2.0 for name in CLASSES})
    summary = {"score": float(score), "stderr": 0.1, "found": found,
               "layouts": {name: {"score": float(score) - k, "stderr": 0.1, "episodes": 64}
                           for k, name in enumerate(CLASSES)}}
    improved = controller.observe(summary, steps)
    return improved, controller.after_eval(steps)


def _climb(controller, rung_scores=(10.0, 8.0, 6.0), per_rung=4):
    """Play every rung but the last: `per_rung` evaluations at its flat score, the gate met at the last of them."""
    index = 0
    for score in rung_scores:
        for turn in range(per_rung):
            _eval(controller, index, score, 0.9 if turn == per_rung - 1 else 0.5)
            yield index, score, turn
            index += 1


def test_a_gate_stepped_ladder_neither_anneals_nor_converges_until_its_last_rung_has_its_own_plateau():
    controller = ConvergenceController(_config(), CLASSES)
    for index, score, turn in _climb(controller):
        assert controller.plateau_env_steps is None and controller.lr_scale(100 * (index + 2)) == 1.0
        assert controller.converged_layouts() == [], (index, score)
    assert controller.fade.rung == 3 and controller.fade.settled
    # The overall best belongs to the rung the ladder is at: nothing of the easy rungs' 10 is left to beat.
    assert controller.tracker.best is None and controller.tracker.history == []

    index = 12
    outcomes = []
    for turn in range(6):
        _, outcome = _eval(controller, index + turn, 4.0, 0.9)
        outcomes.append(outcome.action)
        if turn < 2:
            assert controller.plateau_env_steps is None and controller.converged_layouts() == []
    # Its own plateau: patience 2 evaluations after its first, then the learning rate anneals and the classes converge.
    assert controller.tracker.best == 4.0 and controller.plateau_env_steps == 100 * (index + 3)
    assert controller.lr_scale(100 * (index + 8)) < 1.0
    assert controller.converged_layouts() == list(CLASSES) and outcomes[-1] == ADVANCE


def test_without_the_rebaseline_the_easy_rungs_best_reads_the_top_rung_as_a_plateau_at_once(monkeypatch):
    """The regression this fixes, pinned: with the re-baseline off, the first evaluation at the last rung (4.0 against
    the easy rung's 10.0) is already a plateau and the classes converge on it, with no rung of their own to judge."""
    monkeypatch.setattr(ConvergenceController, "rebaseline", lambda self: None)
    controller = ConvergenceController(_config(), CLASSES)
    for _ in _climb(controller):
        pass
    _eval(controller, 12, 4.0, 0.9)
    assert controller.plateau_env_steps == 1300
    _, outcome = _eval(controller, 13, 4.0, 0.9)
    _, outcome = _eval(controller, 14, 4.0, 0.9)
    assert controller.converged_layouts() == list(CLASSES) and outcome.action == ADVANCE


def test_every_forward_step_clears_the_overall_tracker_the_classes_and_the_plateau():
    controller = ConvergenceController(_config(), CLASSES)
    tracker = controller.tracker
    state = controller.layouts["mage_dps"]
    controller.plateau_env_steps = 150          # as if an easy rung had plateaued, to see the step drop it
    state.converged, state.converged_score, state.converged_margin, state.reentries = True, 5.0, 0.5, 2
    state.scores = [5.0, 5.0, 5.0]
    tracker.observe(10.0, 100)
    controller.best_summary = {"score": 10.0}
    controller.fade.rung = 0
    for index in range(4):
        _eval(controller, index, 10.0, 0.5 if index < 3 else 0.9)
    assert controller.fade.rung == 1
    assert controller.tracker is tracker and tracker.best is None and tracker.history == []
    assert controller.plateau_env_steps is None and controller.best_summary is None
    assert (state.scores, state.converged, state.converged_score, state.converged_margin, state.reentries) == (
        [], False, None, 0.0, 0)
    assert len(state.tracker.history) == 0 and state.played
    assert controller.baselined == {"fade": 1, "costs": 0}


def test_a_plateau_stepped_ladder_behaves_exactly_as_before():
    """M1-like (require_plateau on): a recorded run replayed against what the code before this change produced --
    [eval, improved, rung, plateau_env_steps, lr_scale next, best, evals since best, converged, outcome]."""
    config = _config(rungs=(1.0, 0.5, 0.0), gate_metric="arrived", require_plateau=True)
    config.total_env_steps = 5000
    controller = ConvergenceController(config, CLASSES)
    scores = [2, 4, 6, 8, 8, 8, 8, 8, 7.0, 6.9, 7.1, 7.0, 7.0, 7.0, 7.0, 7.0, 7.0, 7.0, 7.0, 7.0]
    arrived = [.1, .3, .5, .7] + [.85] * 16
    both = list(CLASSES)
    expected = [
        [0, True, 0, None, 1.0, 2.0, 0, [], "continue"],
        [1, True, 0, None, 1.0, 4.0, 0, [], "continue"],
        [2, True, 0, None, 1.0, 6.0, 0, [], "continue"],
        [3, True, 0, None, 1.0, 8.0, 0, [], "continue"],
        [4, False, 0, None, 1.0, 8.0, 1, [], "continue"],
        [5, False, 0, 600, 0.979545, 8.0, 2, [], "continue"],
        [6, False, 1, 600, 0.959091, 8.0, 3, both, "continue"],
        [7, False, 1, 600, 0.938636, 8.0, 4, both, "continue"],
        [8, False, 0, 600, 0.918182, 8.0, 5, [], "continue"],
        [9, False, 0, 600, 0.897727, 8.0, 6, [], "continue"],
        [10, False, 0, 600, 0.877273, 8.0, 7, [], "continue"],
        [11, False, 1, 600, 0.856818, 8.0, 8, both, "continue"],
        [12, False, 1, 600, 0.836364, 8.0, 9, both, "continue"],
        [13, False, 1, 600, 0.815909, 8.0, 10, both, "continue"],
        [14, False, 2, 600, 0.795455, 8.0, 11, both, "advance"],
        [15, False, 2, 600, 0.775, 8.0, 12, both, "advance"],
        [16, False, 2, 600, 0.754545, 8.0, 13, both, "advance"],
        [17, False, 2, 600, 0.734091, 8.0, 14, both, "advance"],
        [18, False, 2, 600, 0.713636, 8.0, 15, both, "advance"],
        [19, False, 2, 600, 0.693182, 8.0, 16, both, "advance"],
    ]
    for index, (score, gate) in enumerate(zip(scores, arrived)):
        steps = 100 * (index + 1)
        controller.observe_update({name: {"approx_kl": 0.001, "entropy": 0.5 * math.log(10.0), "allowed_actions": 10.0}
                                   for name in CLASSES}, controller.lr_scale(steps))
        controller.observe_training_episodes({name: 2.0 for name in CLASSES})
        summary = {"score": float(score), "stderr": 0.1, "arrived": gate,
                   "layouts": {name: {"score": float(score) - k, "stderr": 0.1, "episodes": 64}
                               for k, name in enumerate(CLASSES)}}
        improved = controller.observe(summary, steps)
        outcome = controller.after_eval(steps)
        row = [index, bool(improved), controller.fade.rung, controller.plateau_env_steps,
               round(controller.lr_scale(steps + 100), 6), controller.tracker.best, controller.tracker.evals_since_best,
               controller.converged_layouts(), outcome.action]
        assert row == expected[index]
    assert controller.baselined == {"fade": 0, "costs": 0}


def _old_state(controller: ConvergenceController) -> dict:
    """The controller state as a checkpoint saved before this change held it: the fade at rung 2, the overall plateau set,
    the easy rung's best in both trackers, the classes converged -- and no `baselined`."""
    controller.fade.rung = 2
    controller.plateau_env_steps = 300
    controller.evals = 9
    controller.tracker.observe(10.0, 100)
    for steps in (200, 300, 400):
        controller.tracker.observe(7.0, steps)
    for state in controller.layouts.values():
        state.played, state.converged, state.converged_score, state.converged_margin = True, True, 6.0, 0.5
        state.scores = [6.0, 6.0, 6.0]
        state.tracker.observe(9.0, 100)
    saved = controller.state_dict()
    del saved["baselined"]
    return {"convergence": controller.tracker.state_dict(), "controller": saved, "score_kind": "score_outcome"}


def test_a_checkpoint_saved_the_old_way_above_rung_zero_loads_without_the_easy_rungs_convergence_state():
    old = _old_state(ConvergenceController(_config(), CLASSES))
    assert old["controller"]["plateau_env_steps"] == 300 and old["convergence"]["best"] == 10.0

    again = ConvergenceController(_config(), CLASSES)
    note = restore_evaluation_state(again.tracker, again, old, "score_outcome")
    assert again.fade.rung == 2 and again.plateau_env_steps is None
    assert again.lr_scale(1000) == 1.0
    assert again.tracker.best is None and again.tracker.history == [] and again.tracker.patience == 2
    assert again.converged_layouts() == [] and all(state.played for state in again.layouts.values())
    assert all(state.tracker.best is None and state.scores == [] for state in again.layouts.values())
    assert again.baselined == {"fade": 2, "costs": 0} and not again.stale_ladder
    assert note is not None and "easier rungs" in note

    # A separate tracker object (not the controller's own) is dropped as well.
    other = ConvergenceController(_config(), CLASSES)
    from animus.evaluation import ConvergenceTracker
    separate = ConvergenceTracker(patience=2)
    restore_evaluation_state(separate, other, old, "score_outcome")
    assert separate.best is None and separate.history == []


def test_an_old_checkpoint_at_rung_zero_drops_the_plateau_it_could_not_have_now_but_keeps_its_tracker():
    """M2's 2026-10-07 snapshot: rung 0, the overall plateau set at 30M and the rates at 0.56 -- the old code let a
    gate-stepped fade plateau before its last rung. The tracker is the first rung's own, so it stays."""
    controller = ConvergenceController(_config(), CLASSES)
    saved = _old_state(controller)
    saved["controller"]["fade"]["rung"] = 0
    again = ConvergenceController(_config(), CLASSES)
    assert restore_evaluation_state(again.tracker, again, saved, "score_outcome") is None
    assert again.fade.rung == 0 and again.plateau_env_steps is None and again.lr_scale(1000) == 1.0
    assert again.tracker.best == 10.0 and again.converged_layouts() == []


def test_a_checkpoint_saved_by_this_code_resumes_with_its_rungs_own_state_intact():
    controller = ConvergenceController(_config(), CLASSES)
    for index, score, turn in _climb(controller):
        pass
    for turn in range(4):
        _eval(controller, 12 + turn, 4.0, 0.9)
    assert controller.plateau_env_steps is not None
    saved = {"convergence": controller.tracker.state_dict(), "controller": controller.state_dict(),
             "score_kind": "score_outcome"}
    again = ConvergenceController(_config(), CLASSES)
    assert restore_evaluation_state(again.tracker, again, saved, "score_outcome") is None
    assert again.plateau_env_steps == controller.plateau_env_steps and again.tracker.best == 4.0
    assert again.converged_layouts() == list(CLASSES) and again.baselined == {"fade": 3, "costs": 0}


def test_resumes_of_rung_zero_and_of_plateau_stepped_stages_are_untouched():
    for config in (_config(), _config(require_plateau=True, rungs=(1.0, 0.5, 0.0))):
        controller = ConvergenceController(config, CLASSES)
        controller.plateau_env_steps = 300
        controller.tracker.observe(10.0, 100)
        saved = {"convergence": controller.tracker.state_dict(), "controller": controller.state_dict(),
                 "score_kind": "score_outcome"}
        again = ConvergenceController(config, CLASSES)
        assert restore_evaluation_state(again.tracker, again, saved, "score_outcome") is None
        assert again.plateau_env_steps == 300 and again.tracker.best == 10.0
    # A plateau-stepped ladder above rung 0 from a checkpoint with no `baselined` is as it always was.
    config = _config(require_plateau=True, rungs=(1.0, 0.5, 0.0))
    controller = ConvergenceController(config, CLASSES)
    saved = _old_state(controller)
    again = ConvergenceController(config, CLASSES)
    assert restore_evaluation_state(again.tracker, again, saved, "score_outcome") is None
    assert again.plateau_env_steps == 300 and again.tracker.best == 10.0 and again.converged_layouts() == list(CLASSES)


def test_a_state_with_no_controller_section_still_loads():
    again = ConvergenceController(_config(), CLASSES)
    assert restore_evaluation_state(again.tracker, again, {"score_kind": "score_outcome"}, "score_outcome") is None
    assert again.baselined == {"fade": 0, "costs": 0}


def test_a_rung_conditional_column_with_no_episodes_is_not_a_non_finite_warning(tmp_path):
    names = ["found", "found_room", "rung_room", "found_deep", "rung_deep"]
    # Two episodes, neither placed at the room or deep rung: found_room / found_deep have no denominator.
    episodes = np.array([[1.0, 0.0, 0.0, 0.0, 0.0], [0.0, 0.0, 0.0, 0.0, 0.0]])
    column_means = means(episodes, names)
    assert math.isnan(column_means[1]) and math.isnan(column_means[3])
    assert undefined(episodes, names) == {"found_room", "found_deep"}

    row = {"entropy": 0.5, **{f"episode_{n}": float(v) for n, v in zip(names, column_means)}}
    quiet = json.loads(write_progress(tmp_path, row, {f"episode_{n}" for n in undefined(episodes, names)}).read_text())
    assert quiet["nonfinite"] == "" and quiet["episode_found_room"] is None and quiet["episode_found"] == 0.5

    # A real NaN anywhere else still warns, and so does a NaN in a column that does have its events.
    row["value_loss"] = float("nan")
    row["episode_found"] = float("nan")
    loud = json.loads(write_progress(tmp_path, row, {"episode_found_room", "episode_found_deep"}).read_text())
    assert loud["nonfinite"] == "episode_found,value_loss" or set(loud["nonfinite"].split(",")) == {
        "episode_found", "value_loss"}
    # An undefined column that is infinite (not NaN) is still a fault.
    row = {"episode_found_room": float("inf")}
    assert write_progress(tmp_path, row, {"episode_found_room"}).read_text().count('"nonfinite": "episode_found_room"')


def test_undefined_is_empty_when_every_column_has_events_or_there_are_no_episodes():
    names = ["found", "found_room", "rung_room"]
    assert undefined(np.array([[1.0, 1.0, 1.0]]), names) == set()
    assert undefined(np.zeros((0, 3)), names) == set()


def test_each_rung_left_keeps_its_best_before_the_next_rungs_first_evaluation_overwrites_best_pt(tmp_path):
    """The trainer's order at an evaluation (train.py): observe, save best.pt when improved, archive the rungs left.
    best_rung<k>.pt is the best of rung k, this evaluation's included, and best.pt is then the new rung's."""
    from animus.runs import archive_rung_best
    controller = ConvergenceController(_config(), CLASSES)
    best = tmp_path / "best.pt"
    steps = []
    for index in range(12):
        score = (10.0, 8.0, 6.0)[index // 4] + 1.0 * (index % 4)       # rising within each rung, past the margin
        improved, _ = _eval(controller, index, score, 0.9 if index % 4 == 3 else 0.5)
        if improved:
            best.write_text(f"eval{index}")
        for ladder, rung in controller.rung_exits:
            steps.append((index, ladder, rung))
            archive_rung_best(tmp_path, ladder, rung, best)
    assert steps == [(3, "fade", 0), (7, "fade", 1), (11, "fade", 2)]
    # The stepping evaluation itself was the rung's best, and is in its archive.
    assert [(tmp_path / f"best_rung{k}.pt").read_text() for k in range(3)] == ["eval3", "eval7", "eval11"]
    # The first evaluation at the new rung is a new best of its own (the tracker was re-baselined).
    controller2 = ConvergenceController(_config(), CLASSES)
    for index in range(4):
        _eval(controller2, index, 10.0, 0.9 if index == 3 else 0.5)
    assert controller2.rung_exits == [("fade", 0)]
    improved, _ = _eval(controller2, 4, 5.0, 0.5)
    assert improved and controller2.rung_exits == []


def test_a_plateau_stepped_ladder_exits_no_rung():
    config = _config(rungs=(1.0, 0.5, 0.0), gate_metric="found", require_plateau=True)
    controller = ConvergenceController(config, CLASSES)
    stepped, highest = [], 0
    for index, score in enumerate([2, 4, 6, 8] + [8.0] * 16):
        _eval(controller, index, float(score), 0.9)
        stepped += controller.rung_exits
        highest = max(highest, controller.fade.rung)
    assert highest > 0 and stepped == []     # it did step down, and exited nothing


def test_the_trainer_archives_straight_after_the_best_save_in_the_evaluation():
    from pathlib import Path
    source = (Path(__file__).resolve().parents[1] / "animus" / "train.py").read_text()
    observed = source.index("improved = controller.observe(summary, self.env_steps)")
    best_save = source.index("self._save(self.best_path)", observed)
    archive = source.index("archive_rung_best(self.run_dir", best_save)
    assert archive - best_save < 800 and "_save(" not in source[best_save + 10:archive]
