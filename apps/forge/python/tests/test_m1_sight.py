"""M1 controls, redesigned (perception-goals REDESIGN section 1; the sim's SightEncounter): the stage config reads its
measures -- arrived, and arrived without and with the compass -- the per-compass rates are averaged over their own
episodes, the shaping fade and the cost ladder are gated on the arrival at the rung's own withholding mix, and the new
M1 seeds from the current M1's checkpoint with the move and compass blocks carried by name."""

import dataclasses
import json
import os
import re
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest
import torch

from animus.bootstrap import MOVE_REVISION_4_COLUMNS, seed_trainer
from animus.config import TrainConfig
from animus.episode_means import PER_EVENT, means
from animus.evaluation import EvalResult
from animus.mappo.trainer import MappoConfig, MappoTrainer
from animus.protocol import Layout
from animus.stages import block_spans

CONFIGS = Path(__file__).resolve().parents[1] / "configs"
CURRICULUM = Path(__file__).resolve().parents[4] / "src" / "server" / "game" / "Animus" / "Scenario" / "Curriculum"
CHECKPOINT = Path(os.environ.get("FORGE_M1_CHECKPOINT",
                                 "/azerothcore/var/animus-forge/shared/runs/move1_controls/latest.pt"))

COMPASS = ["objective", "objective_bearing_sin", "objective_bearing_cos", "objective_distance", "objective_near",
           "detour"]
MOVE_5 = [name for name in MOVE_REVISION_4_COLUMNS if name not in COMPASS]


def sim_columns() -> set[str]:
    """Every episode info column M1's sim can report: the scenario's own, SightEncounter's and the reward terms'."""
    names = set(re.findall(r'_info\.Add\("(\w+)"', (CURRICULUM / "StageScenario.cpp").read_text()))
    names |= set(re.findall(r'table\.Add\("(\w+)"', (CURRICULUM / "Encounters" / "SightEncounter.cpp").read_text()))
    names |= {"reward_" + name for name in re.findall(r'return "(\w+)";',
                                                       (CURRICULUM / "Rewards" / "CombatReward.cpp").read_text())}
    return names


def test_the_stage_config_reads_its_measures_and_gates_on_the_rungs_arrival():
    config = TrainConfig.load(CONFIGS / "move1_controls.yaml")
    assert config.run_name == "move1_controls"
    headline = config.status.headline
    assert headline[0] == "arrived"
    assert {"arrived_no_compass", "arrived_with_compass", "compass_withheld"} <= set(headline)
    # M1's measures and targets kept: the stop's precision, the overshoot, the time ratio, the course kinks.
    for kept in ("arrive_seconds", "time_ratio", "stop_distance", "overshoot", "course_kinks"):
        assert kept in headline and kept in config.status.targets
    assert set(config.status.targets) <= set(headline)
    assert config.status.targets["stop_distance"] == "<= 0.5"
    # Every column the headline and the evaluation name is one the sim reports (or the evaluation derives).
    known = sim_columns()
    missing = [name for name in (*headline, *config.eval.report) if name not in known]
    assert not missing, missing
    # The convergence is the stage's own measure; the ladders step on the arrival at the rung's own mix.
    assert config.convergence.measure == "arrived"
    assert config.fade.enabled and config.fade.gate_metric == "arrived_at_rung"
    assert config.fade.rungs == (1.0, 0.5, 0.25, 0.0)
    assert config.costs.gate_metric == "arrived_at_rung"
    # The evaluation: whole passes over the 32 pairs, each with and without the compass.
    assert config.eval.episodes % 64 == 0
    assert {"arrived_no_compass", "arrived_with_compass", "compass_withheld"} <= set(config.eval.report)


def test_the_fades_scales_are_the_sims_withholding_rungs():
    """SightDraw::FADE_SCALES is what the fade sends: a rung the sim does not know would withhold by interpolation."""
    draw = (CURRICULUM / "Encounters" / "SightDraw.h").read_text()
    scales = re.search(r"FADE_SCALES = \{ ([^}]*) \}", draw).group(1)
    assert tuple(float(v.strip().rstrip("f")) for v in scales.split(",")) == \
        TrainConfig.load(CONFIGS / "move1_controls.yaml").fade.rungs


COLUMNS = ("arrived", "compass_withheld", "compass_present", "compass_withhold_chance", "arrived_no_compass",
           "arrived_with_compass")


def episode(arrived: bool, withheld: bool, chance: float) -> list[float]:
    return [float(arrived), float(withheld), float(not withheld), chance, float(arrived and withheld),
            float(arrived and not withheld)]


def test_the_compass_rates_are_averaged_over_their_own_episodes():
    """arrived_no_compass is the arrival rate in the episodes that withheld the compass, not over every episode: a
    0.6 rung's training means read it over its 60%."""
    assert PER_EVENT["arrived_no_compass"] == "compass_withheld"
    assert PER_EVENT["arrived_with_compass"] == "compass_present"
    rows = np.array([episode(True, True, 0.6), episode(False, True, 0.6), episode(False, True, 0.6),
                     episode(True, False, 0.6), episode(True, False, 0.6)], dtype=np.float32)
    out = dict(zip(COLUMNS, means(rows, COLUMNS)))
    assert out["arrived_no_compass"] == pytest.approx(1.0 / 3.0)
    assert out["arrived_with_compass"] == pytest.approx(1.0)
    assert out["compass_withheld"] == pytest.approx(0.6)
    assert out["arrived"] == pytest.approx(0.6)


def test_arrived_at_rung_weighs_the_evaluation_by_the_rungs_mix():
    """The evaluation plays half its episodes without the compass whatever the rung: at the first rung (chance 0) a
    policy that never saw an absent compass arrives 95% with it and 0% without, 0.475 overall -- short of a 0.8 gate it
    has in fact passed. arrived_at_rung is its arrival at the rung's own mix."""
    rows = [episode(True, False, 0.0)] * 19 + [episode(False, False, 0.0)] + [episode(False, True, 0.0)] * 20
    result = EvalResult("learner", np.zeros(len(rows)), np.array(rows, dtype=np.float32), COLUMNS)
    summary = result.summary(COLUMNS)
    assert summary["arrived"] == pytest.approx(0.475)
    assert summary["arrived_with_compass"] == pytest.approx(0.95)
    assert summary["arrived_no_compass"] == pytest.approx(0.0)
    assert summary["arrived_at_rung"] == pytest.approx(0.95)
    # At the 0.6 rung, 60% of it is the arrival without.
    rows = [episode(True, False, 0.6)] * 10 + [episode(True, True, 0.6)] * 5 + [episode(False, True, 0.6)] * 5
    summary = EvalResult("learner", np.zeros(len(rows)), np.array(rows, dtype=np.float32), COLUMNS).summary(COLUMNS)
    assert summary["arrived_at_rung"] == pytest.approx(0.6 * 0.5 + 0.4 * 1.0)
    # Per layout too (the classes' own gate readings): the warrior's every other row, 3 of 5 without and 5 of 5 with.
    split = EvalResult("learner", np.zeros(len(rows)), np.array(rows, dtype=np.float32), COLUMNS,
                       layouts=("warrior", "mage") * 10)
    assert split.summary(COLUMNS)["layouts"]["warrior"]["arrived_at_rung"] == pytest.approx(0.6 * 0.6 + 0.4 * 1.0)
    # A group with no episode without the compass has no reading at a rung that withholds it.
    shown_only = EvalResult("learner", np.zeros(10), np.array(rows[:10], dtype=np.float32), COLUMNS)
    assert shown_only.summary(COLUMNS)["arrived_at_rung"] is None
    # A stage without the compass columns has no such reading.
    plain = EvalResult("learner", np.zeros(2), np.array([[1.0], [0.0]], dtype=np.float32), ("arrived",))
    assert "arrived_at_rung" not in plain.summary(("arrived",))


def redesigned(stage: dict) -> dict:
    """The new M1's stage.json from the current M1's: the move block at revision 4 (the compass inside it, unnamed)
    becomes revision 5 and the compass, the same 63 columns in the same place, named; everything else as it was. A
    checkpoint already past the split is returned as it is."""
    out = json.loads(json.dumps(stage))
    for entry in out["layouts"].values():
        blocks = []
        for block in entry["blocks"]:
            if block["name"] == "move" and int(block.get("revision", 0)) == 4:
                first, width = block["obs"]
                assert width == len(MOVE_REVISION_4_COLUMNS)
                moved = dict(block, revision=5, obs=[first, len(MOVE_5)], obs_names=list(MOVE_5))
                compass = {"name": "compass", "revision": 1, "obs": [first + len(MOVE_5), len(COMPASS)],
                           "actions": [block["actions"][0] + block["actions"][1], 0], "obs_names": list(COMPASS)}
                blocks += [moved, compass]
            else:
                blocks.append(block)
        entry["blocks"] = blocks
    return out


def column(stage: dict, layout: str, block: str, name: str) -> int:
    for entry in stage["layouts"][layout]["blocks"]:
        if entry["name"] == block:
            names = entry.get("obs_names") or list(MOVE_REVISION_4_COLUMNS)
            return entry["obs"][0] + names.index(name)
    raise KeyError(block)


@pytest.mark.skipif(not CHECKPOINT.is_file(), reason=f"no M1 checkpoint at {CHECKPOINT}")
def test_the_new_m1_seeds_from_the_current_m1_checkpoint_by_name():
    checkpoint = torch.load(CHECKPOINT, map_location="cpu", weights_only=False)
    old_stage = checkpoint.get("stage")
    if not old_stage:
        old_stage = json.loads((CHECKPOINT.parent / "stage.json").read_text())
        checkpoint["stage"] = old_stage
    new_stage = redesigned(old_stage)
    saved = checkpoint["config"]["mappo"]
    fields = {field.name for field in dataclasses.fields(MappoConfig)}
    config = MappoConfig(**{key: tuple(value) if isinstance(value, list) else value for key, value in saved.items()
                            if key in fields})
    names = [layout["name"] for layout in checkpoint["spec"]["layouts"]]
    layouts = [(layout["obs_dim"], layout["num_actions"]) for layout in checkpoint["spec"]["layouts"]]
    # No camera in the new networks here: the redesign's camera is revision 5 and starts fresh anyway, and what this
    # checks is the movement (the adapter columns, their normalisers, the move block's actions).
    torch.manual_seed(0)
    trainer = MappoTrainer(layouts, checkpoint["spec"]["state_dim"], config)
    spec = SimpleNamespace(layouts=tuple(Layout(name, *dims) for name, dims in zip(names, layouts)),
                           state_dim=checkpoint["spec"]["state_dim"])
    assert seed_trainer(trainer, checkpoint, spec, new_stage) == names

    old = checkpoint["trainer"]
    new = {"actor": trainer.actor.state_dict(), "critic": trainer.critic.state_dict()}
    for index, name in enumerate(names):
        move = block_spans(new_stage, name)["move"]
        for network in ("actor", "critic"):
            new_w, old_w = new[network][f"adapters.{index}.weight"], old[network][f"adapters.{index}.weight"]
            # The move block's kept columns and the compass's, each where the old move block had it by name.
            for block, columns in (("move", MOVE_5), ("compass", COMPASS)):
                for column_name in columns:
                    to = column(new_stage, name, block, column_name)
                    at = column(old_stage, name, "move", column_name)
                    torch.testing.assert_close(new_w[:, to], old_w[:, at])
                    for stat in ("mean", "var"):
                        assert float(new[network][f"norms.{index}.{stat}"][to]) == \
                            float(old[network][f"norms.{index}.{stat}"][at]), (name, network, column_name, stat)
        # The move block's actions are its own, as they were.
        first, count = move[1]
        head_new, head_old = new["actor"][f"heads.{index}.weight"], old["actor"][f"heads.{index}.weight"]
        torch.testing.assert_close(head_new[first:first + count], head_old[first:first + count])
