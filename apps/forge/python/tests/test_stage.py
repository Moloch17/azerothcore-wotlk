"""The convergence rule that ends every stage (animus.stage.ConvergenceController)."""

import math

import pytest

from animus.config import TrainConfig
from animus.stage import ADVANCE, CONTINUE, ConvergenceController

pytest.importorskip("torch")

from animus.mappo.trainer import MappoTrainer  # noqa: E402

CLASSES = ("warrior_dps", "mage_dps")


def _config(**overrides) -> TrainConfig:
    config = TrainConfig()
    config.total_env_steps = 1000
    config.eval.every_env_steps = 100
    config.convergence.patience = 2
    config.convergence.window = 3
    config.convergence.kl = 0.003
    config.convergence.min_improvement_abs = 0.5
    config.convergence.min_improvement = 0.0
    config.entropy_floor.fraction = 0.0
    for key, value in overrides.items():
        section, _, name = key.partition(".")
        setattr(getattr(config, section), name, value) if name else setattr(config, section, value)
    return config


def _summary(scores: dict[str, float], stderr: float = 0.0) -> dict:
    return {"score": sum(scores.values()) / len(scores), "stderr": stderr,
            "layouts": {name: {"score": score, "stderr": stderr, "episodes": 64} for name, score in scores.items()}}


def _run(controller: ConvergenceController, evals: list[dict[str, float]], kl: float = 0.001, entropy: float = 0.5,
         rung: float | None = None, allowed: float = 10.0) -> list:
    """Feed `evals` evaluations, each preceded by one update carrying the given per-class signals."""
    outcomes = []
    for index, scores in enumerate(evals):
        env_steps = 100 * (index + 1)
        controller.observe_update({name: {"approx_kl": kl, "entropy": entropy * math.log(allowed),
                                          "allowed_actions": allowed} for name in CLASSES}, 1.0)
        if rung is not None:
            controller.observe_training_episodes({name: rung for name in CLASSES})
        controller.observe(_summary(scores), env_steps)
        outcomes.append(controller.after_eval(env_steps))
    return outcomes


def test_a_stage_advances_once_every_class_shows_all_four_signals():
    controller = ConvergenceController(_config(), CLASSES)
    flat = {"warrior_dps": 5.0, "mage_dps": 3.0}
    outcomes = _run(controller, [flat] * 4, rung=2.0)
    assert outcomes[-1].action == ADVANCE and outcomes[-1].reason == "converged"
    assert controller.converged_layouts() == list(CLASSES)
    assert all(row["missing"] == [] for row in outcomes[-1].report.values())


def test_a_climbing_score_blocks_convergence():
    controller = ConvergenceController(_config(), CLASSES)
    climbing = [{"warrior_dps": 5.0 + index, "mage_dps": 3.0} for index in range(5)]
    outcomes = _run(controller, climbing)
    assert all(outcome.action == CONTINUE for outcome in outcomes)
    assert "score" in controller.report()["warrior_dps"]["missing"]
    assert controller.weakest()[0] == "warrior_dps"


def test_a_moving_policy_blocks_convergence():
    controller = ConvergenceController(_config(), CLASSES)
    flat = {"warrior_dps": 5.0, "mage_dps": 3.0}
    outcomes = _run(controller, [flat] * 5, kl=0.02)
    assert all(outcome.action == CONTINUE for outcome in outcomes)
    assert controller.report()["mage_dps"]["missing"] == ["kl"]


def test_the_kl_is_read_against_the_learning_rate():
    """A KL that only fell with the anneal is the schedule: at a tenth of the rate 0.0005 is 0.005."""
    controller = ConvergenceController(_config(), CLASSES)
    flat = {"warrior_dps": 5.0, "mage_dps": 3.0}
    for index in range(4):
        controller.observe_update({name: {"approx_kl": 0.0005, "entropy": 1.0, "allowed_actions": 10.0}
                                   for name in CLASSES}, 0.1)
        controller.observe(_summary(flat), 100 * (index + 1))
    assert controller.after_eval(400).action == CONTINUE
    assert "kl" in controller.report()["warrior_dps"]["missing"]


def test_a_sliding_entropy_blocks_convergence():
    controller = ConvergenceController(_config(), CLASSES)
    flat = {"warrior_dps": 5.0, "mage_dps": 3.0}
    for index in range(4):
        controller.observe_update({name: {"approx_kl": 0.001, "entropy": (0.9 - 0.1 * index) * math.log(10.0),
                                          "allowed_actions": 10.0} for name in CLASSES}, 1.0)
        controller.observe(_summary(flat), 100 * (index + 1))
    assert controller.after_eval(400).action == CONTINUE
    assert controller.report()["warrior_dps"]["missing"] == ["entropy"]


def test_a_collapsed_entropy_blocks_convergence_when_a_floor_is_set():
    controller = ConvergenceController(_config(**{"entropy_floor.fraction": 0.3}), CLASSES)
    flat = {"warrior_dps": 5.0, "mage_dps": 3.0}
    outcomes = _run(controller, [flat] * 4, entropy=0.1)
    assert outcomes[-1].action == CONTINUE and controller.report()["warrior_dps"]["missing"] == ["entropy"]


def test_a_climbing_ladder_blocks_convergence():
    controller = ConvergenceController(_config(), CLASSES)
    flat = {"warrior_dps": 5.0, "mage_dps": 3.0}
    for index in range(4):
        controller.observe_update({name: {"approx_kl": 0.001, "entropy": 1.0, "allowed_actions": 10.0}
                                   for name in CLASSES}, 1.0)
        controller.observe_training_episodes({name: float(index) for name in CLASSES})
        controller.observe(_summary(flat), 100 * (index + 1))
    assert controller.after_eval(400).action == CONTINUE
    assert controller.report()["warrior_dps"]["missing"] == ["ladder"]


def test_the_weakest_class_decides_and_the_converged_one_is_held():
    controller = ConvergenceController(_config(), CLASSES)
    evals = [{"warrior_dps": 5.0, "mage_dps": 3.0 + index} for index in range(5)]
    outcomes = _run(controller, evals)
    assert all(outcome.action == CONTINUE for outcome in outcomes)
    assert controller.converged_layouts() == ["warrior_dps"] and controller.active_layouts() == ["mage_dps"]
    assert controller.hold_weights() == {"warrior_dps": 0.02, "mage_dps": 1.0}


def test_a_held_class_re_enters_when_it_regresses():
    controller = ConvergenceController(_config(), CLASSES)
    flat = {"warrior_dps": 5.0, "mage_dps": 3.0}
    _run(controller, [flat] * 4)
    assert controller.converged_layouts() == list(CLASSES)
    _run(controller, [{"warrior_dps": 2.0, "mage_dps": 3.0}])
    assert controller.converged_layouts() == ["mage_dps"]
    assert controller.report()["warrior_dps"]["reentries"] == 1


def test_the_budget_advances_and_names_who_was_not_done():
    controller = ConvergenceController(_config(), CLASSES)
    _run(controller, [{"warrior_dps": 5.0 + index, "mage_dps": 3.0} for index in range(2)])
    outcome = controller.at_budget()
    assert outcome.action == ADVANCE and outcome.reason == "budget"
    assert outcome.report["warrior_dps"]["converged"] is False


def test_a_class_the_run_never_plays_is_not_waited_for():
    controller = ConvergenceController(_config(), (*CLASSES, "director"))
    flat = {"warrior_dps": 5.0, "mage_dps": 3.0}
    outcomes = _run(controller, [flat] * 4)
    assert outcomes[-1].action == ADVANCE
    assert controller.report()["director"]["missing"] == ["never played"]


def test_the_learning_rate_is_held_until_the_score_plateaus():
    config = _config()
    config.mappo.lr_final_fraction = 0.1
    controller = ConvergenceController(config, CLASSES)
    assert controller.lr_scale(500) == 1.0
    _run(controller, [{"warrior_dps": 5.0, "mage_dps": 3.0}] * 3)
    assert controller.plateau_env_steps == 300
    assert controller.lr_scale(300) == pytest.approx(1.0)
    assert controller.lr_scale(1000) == pytest.approx(0.1)
    assert 0.1 < controller.lr_scale(650) < 1.0

    config.convergence.lr_hold_until_plateau = False
    plain = ConvergenceController(config, CLASSES)
    assert plain.lr_scale(500) == pytest.approx(0.55)


def test_state_round_trips_through_a_checkpoint():
    controller = ConvergenceController(_config(), CLASSES)
    _run(controller, [{"warrior_dps": 5.0, "mage_dps": 3.0 + index} for index in range(3)])
    saved = controller.state_dict()
    restored = ConvergenceController(_config(), CLASSES)
    restored.load_state_dict(saved)
    assert restored.converged_layouts() == controller.converged_layouts()
    assert restored.report() == controller.report()


def test_entropy_floor_lifts_a_collapsing_policy_and_lets_go():
    config = _config(**{"entropy_floor.fraction": 0.5, "entropy_floor.max_boost": 4.0, "entropy_floor.rate": 0.5})
    controller = ConvergenceController(config, CLASSES)
    base = controller.entropy_coef(0)
    for _ in range(10):
        controller.observe_entropy(0.1, 10.0)  # far under 0.5 x ln(10)
    assert controller.entropy_coef(0) > base
    for _ in range(20):
        controller.observe_entropy(2.0, 10.0)
    assert controller.entropy_coef(0) == pytest.approx(base, rel=0.05)


def test_entropy_coef_follows_its_schedule():
    config = _config()
    config.mappo.entropy_final_fraction = 0.5
    controller = ConvergenceController(config, CLASSES)
    assert controller.entropy_coef(0) == pytest.approx(config.mappo.entropy_coef)
    assert controller.entropy_coef(1000) == pytest.approx(config.mappo.entropy_coef * 0.5)


def test_frozen_layouts_take_no_gradient_and_thaw_again():
    trainer = MappoTrainer([(3, 2), (2, 2)], 3, _config().mappo, "cpu", "cpu")
    trainer.freeze_layouts({1})
    frozen = {name for name, param in trainer.actor.named_parameters() if not param.requires_grad}
    assert frozen and all(".1." in name for name in frozen)
    assert all(param.requires_grad for name, param in trainer.actor.named_parameters() if ".0." in name)
    trainer.freeze_layouts(set())
    assert all(param.requires_grad for param in trainer.actor.parameters())
