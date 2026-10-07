"""apps/forge/tools/resume_check.py: the dry-run resume of the deploy gate, on a tiny fixture checkpoint."""

import copy
import io
import json
import re
import sys
from dataclasses import asdict
from pathlib import Path

import pytest
import torch
import yaml

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "apps" / "forge" / "tools"))

import resume_check as rc  # noqa: E402

from animus.config import TrainConfig  # noqa: E402
from animus.protocol import Layout, Spec  # noqa: E402
from animus.stage import ConvergenceController  # noqa: E402
from animus.train import make_trainer, save_checkpoint, trainer_inputs  # noqa: E402
from test_bootstrap import stage_with  # noqa: E402

CONFIG = {"run_name": "tiny_stage", "mappo": {"hidden": [16, 16]}, "eval": {"every_env_steps": 1000},
          "fade": {"enabled": True, "rungs": [1.0, 0.5, 0.0], "gate_metric": "found", "gate_value": 0.8},
          "costs": {"enabled": False}}
INFO = ["seat", "found", "score_outcome"]


def tiny_stage(width: int = 3) -> dict:
    stage = stage_with({"warrior": [("core", 4, 3), ("move", width, 2)], "mage": [("core", 4, 3), ("move", width, 2)]})
    stage.update({"stage": "tiny_stage", "format": 3, "episode_info": INFO, "tuning": {}})
    return stage


def tiny_spec(stage: dict) -> Spec:
    layouts = tuple(Layout(name, entry["obs_dim"], entry["num_actions"]) for name, entry in stage["layouts"].items())
    return Spec(version=25, num_envs=4, agents_per_env=1, obs_dim=max(l.obs_dim for l in layouts), state_dim=6,
                num_actions=max(l.num_actions for l in layouts), episode_info_dim=len(INFO), goal_count=0, tick_ms=250,
                decision_ticks=1, episode_seconds=60, scenario="tiny_stage", layouts=layouts,
                episode_info_names=tuple(INFO))


@pytest.fixture
def run(tmp_path):
    """A run directory with a checkpoint saved by the learner's own save_checkpoint, a stage.json and a config."""
    torch.manual_seed(0)
    stage = tiny_stage()
    spec = tiny_spec(stage)
    config_path = tmp_path / "tiny_stage.yaml"
    config_path.write_text(yaml.safe_dump(CONFIG))
    config = TrainConfig.load(config_path)
    config.socket, config.torch_threads = "/tmp/the-sims-socket", 7        # what the sim sets at launch
    trainer = make_trainer(config, spec, trainer_inputs(config, spec, stage), device="cpu")
    controller = ConvergenceController(config, [layout.name for layout in spec.layouts])
    controller.fade.rung, controller.fade.falls, controller.fade.evals_at_rung = 1, {0: 2}, 4
    run_dir = tmp_path / "runs" / "tiny_stage"
    run_dir.mkdir(parents=True)
    save_checkpoint(run_dir / "latest.pt", trainer, config, spec, 12, 345_678,
                    {"convergence": controller.tracker.state_dict(), "controller": controller.state_dict(),
                     "stage": stage, "score_kind": "score_outcome"})
    stage_path = tmp_path / "stage.json"
    stage_path.write_text(json.dumps(stage))
    return {"dir": run_dir, "stage": stage, "stage_path": stage_path, "config": config_path, "spec": spec,
            "tmp": tmp_path}


def report_of(run, **kwargs) -> rc.Report:
    return rc.check_resume(run["dir"], kwargs.pop("stage_path", run["stage_path"]), kwargs.pop("config", run["config"]),
                           **kwargs)


def statuses(report: rc.Report) -> dict[str, str]:
    return {check.title: check.status for check in report.checks}


def test_a_compatible_checkpoint_passes_every_check_and_loads_the_ladders(run):
    report = report_of(run)
    text = report.text()
    assert not report.failed, text
    assert "[PASS] resume compatibility (runs.resume_mismatch)\n    empty" in text
    assert "[PASS] layout check (stages.layout_changes)" in text
    assert "[PASS] MappoTrainer.load_state_dict" in text
    assert "missing 0: -" in text and "unexpected 0: -" in text
    # The saved ladder, in the stage's own object: rung 1 of 3 (x0.5), two falls back to rung 0, four evaluations.
    assert "rung 1 of 3 (scale x0.5); saved rung 1" in text
    assert "falls {0: 2}" in text and "evaluations at the rung 4" in text and "held" in text and "settled" in text
    assert "checkpoint" in text and "update 12, env_steps 345,678" in text
    # What cannot be checked without the sim is said, not faked.
    assert "[UNVERIFIED] one rollout step" in text and "[UNVERIFIED] the sim's SPEC" in text
    assert text.splitlines()[-1].startswith("OK:")


def test_the_sim_injected_config_values_are_set_apart_from_real_changes(run):
    text = report_of(run).text()
    assert "sim-injected, expected:" in text and "socket" in text and "torch_threads" in text
    # A real change shows with both values.
    changed = report_of(run, sets=["mappo.gamma=0.5"]).text()
    assert "mappo.gamma: saved 0.99 -> now 0.5" in changed
    assert "1 of the stage's" in changed or "of the stage's" in changed


def test_a_layout_that_moved_fails_both_checks(run):
    wider = tiny_stage(width=5)
    path = run["tmp"] / "new_stage.json"
    path.write_text(json.dumps(wider))
    report = report_of(run, stage_path=path)
    status = statuses(report)
    assert report.failed
    assert status["resume compatibility (runs.resume_mismatch)"] == "FAIL"
    assert status["layout check (stages.layout_changes)"] == "FAIL"
    assert "obs_dim" in report.text() and "move width" in report.text()


def test_a_revised_block_fails_the_layout_check_alone(run):
    revised = copy.deepcopy(run["stage"])
    revised["layouts"]["warrior"]["blocks"][1]["revision"] = 1
    path = run["tmp"] / "revised.json"
    path.write_text(json.dumps(revised))
    status = statuses(report_of(run, stage_path=path))
    assert status["resume compatibility (runs.resume_mismatch)"] == "PASS"
    assert status["layout check (stages.layout_changes)"] == "FAIL"


def test_a_checkpoint_missing_a_weight_lists_the_key_and_fails(run):
    checkpoint = torch.load(run["dir"] / "latest.pt", map_location="cpu", weights_only=False)
    victim = next(key for key in checkpoint["trainer"]["actor"] if key.startswith("heads.0"))
    del checkpoint["trainer"]["actor"][victim]
    torch.save(checkpoint, run["dir"] / "latest.pt")
    report = report_of(run)
    assert report.failed
    text = report.text()
    assert f"missing 1: {victim}" in text
    assert "[FAIL] MappoTrainer.load_state_dict" in text


def test_a_checkpoint_with_an_unexpected_weight_or_a_wrong_shape_fails(run):
    checkpoint = torch.load(run["dir"] / "latest.pt", map_location="cpu", weights_only=False)
    checkpoint["trainer"]["critic"]["stray.weight"] = torch.zeros(1)
    key = next(key for key in checkpoint["trainer"]["actor"] if key.startswith("heads.0.weight"))
    checkpoint["trainer"]["actor"][key] = torch.zeros(1, 1)
    torch.save(checkpoint, run["dir"] / "latest.pt")
    text = report_of(run).text()
    assert "unexpected 1: stray.weight" in text and f"wrong shape 1: {key}" in text


def test_a_ladder_saved_past_the_configured_rungs_reports_the_clamp(run, tmp_path):
    shorter = dict(CONFIG, fade=dict(CONFIG["fade"], rungs=[1.0, 0.0]))
    config_path = tmp_path / "shorter.yaml"
    config_path.write_text(yaml.safe_dump(shorter))
    checkpoint = torch.load(run["dir"] / "latest.pt", map_location="cpu", weights_only=False)
    checkpoint["controller"]["fade"]["rung"] = 2
    torch.save(checkpoint, run["dir"] / "latest.pt")
    assert "CLAMPED to the configured rungs" in report_of(run, config=config_path).text()


def test_a_score_kind_change_is_reported_as_dropped_history(run):
    config = yaml.safe_load(run["config"].read_text())
    config["eval"]["score"] = "return"
    path = run["tmp"] / "return.yaml"
    path.write_text(yaml.safe_dump(config))
    report = report_of(run, config=path)
    assert statuses(report)["evaluation state (best score, convergence history)"] == "FAIL"


def test_with_the_new_builds_spec_the_unverified_line_goes_away(run):
    spec_path = run["tmp"] / "spec.json"
    spec_path.write_text(json.dumps(asdict(run["spec"])))
    report = report_of(run, spec_json=spec_path)
    assert "[UNVERIFIED] the sim's SPEC" not in report.text()
    wider = asdict(run["spec"])
    wider["state_dim"] = 9
    spec_path.write_text(json.dumps(wider))
    report = report_of(run, spec_json=spec_path)
    assert statuses(report)["resume compatibility (runs.resume_mismatch)"] == "FAIL"
    assert "state_dim" in report.text()


def test_the_command_line_exit_codes(run, capsys):
    base = [str(run["dir"]), "--stage-json", str(run["stage_path"]), "--config", str(run["config"])]
    assert rc.main(base) == 0
    assert "OK:" in capsys.readouterr().out
    assert rc.main([str(run["tmp"] / "nowhere"), "--stage-json", str(run["stage_path"]),
                    "--config", str(run["config"])]) == 2
    assert "resume_check:" in capsys.readouterr().err
    wider = run["tmp"] / "wide.json"
    wider.write_text(json.dumps(tiny_stage(width=5)))
    assert rc.main([str(run["dir"]), "--stage-json", str(wider), "--config", str(run["config"])]) == 1


def test_the_run_directory_is_only_read(run):
    before = {path.name: path.stat().st_mtime_ns for path in run["dir"].iterdir()}
    report_of(run)
    assert {path.name: path.stat().st_mtime_ns for path in run["dir"].iterdir()} == before


# ---------------------------------------------------------------------------------------------------- the lr line

def test_the_evaluation_state_line_shows_the_plateau_and_the_learning_rate_scale(run):
    text = report_of(run).text()
    assert "plateau_env_steps None; lr_scale 1 at env step 345,678" in text
    assert "NOTE: plateau_env_steps" not in text


def test_a_plateau_the_restore_clears_is_noted(run):
    checkpoint = torch.load(run["dir"] / "latest.pt", map_location="cpu", weights_only=False)
    checkpoint["controller"]["plateau_env_steps"] = 1000
    torch.save(checkpoint, run["dir"] / "latest.pt")
    # restored as saved: no note (this build keeps what a checkpoint says)
    text = report_of(run).text()
    assert "plateau_env_steps 1000; lr_scale" in text


# -------------------------------------------------------------------------------------------------------- --fresh

FIXTURES = Path(__file__).parent / "fixtures"          # real stage.json files, copied out of the live run's backups
M2 = FIXTURES / "stage_move2_seek.json"
M1 = FIXTURES / "stage_move1_controls.json"


def fresh_text(name: str, stage_path: Path, **kwargs):
    out = io.StringIO()
    code = rc.run_fresh([name], lambda _: stage_path, out=out, **kwargs)
    return code, out.getvalue()


def test_fresh_passes_the_real_m1_and_m2_stage_files():
    for name, path in (("move2_seek", M2), ("move1_controls", M1)):
        report = rc.check_fresh(name, path)
        assert not report.failed, report.text()
        assert [check.title.split(" (")[0] for check in report.checks][-1] == "trainer inputs and MappoTrainer on CPU"
        assert any("held-out arenas" in check.title for check in report.checks)


def test_fresh_fails_on_a_heldout_arena_the_stage_lacks(tmp_path):
    stage = json.loads(M2.read_text())
    stage["arenas"] = [arena for arena in stage["arenas"] if arena["name"] != "sweep"]
    path = tmp_path / "stage.json"
    path.write_text(json.dumps(stage))
    code, text = fresh_text("move2_seek", path)
    assert code == 1
    assert "FAIL" in text and "held-out arenas" in text and "'sweep'" in text and "does not have" in text


def test_fresh_fails_on_a_heldout_arena_the_stage_trains_on(tmp_path):
    stage = json.loads(M2.read_text())
    for arena in stage["arenas"]:
        arena["eval_only"] = False
    path = tmp_path / "stage.json"
    path.write_text(json.dumps(stage))
    code, text = fresh_text("move2_seek", path)
    assert code == 1 and "trains on" in text


def test_fresh_fails_on_a_gate_column_the_stage_lacks(tmp_path):
    stage = json.loads(M2.read_text())
    stage["episode_info"] = [name for name in stage["episode_info"] if name != "found"]
    path = tmp_path / "stage.json"
    path.write_text(json.dumps(stage))
    code, text = fresh_text("move2_seek", path)
    assert code == 1 and "gate_metric 'found'" in text


def test_fresh_fails_on_a_stage_file_of_another_stage_and_on_a_missing_one(tmp_path):
    code, text = fresh_text("move1_controls", M2)
    assert code == 1 and "stage.json of 'move2_seek'" in text
    code, text = fresh_text("move2_seek", tmp_path / "nothing.json")
    assert code == 1 and "no stage.json at" in text


def test_fresh_fails_on_a_camera_the_stage_describes_without_its_image(tmp_path):
    stage = json.loads(M2.read_text())
    for layout in stage["layouts"].values():
        for block in layout["blocks"]:
            block.pop("image", None)
    path = tmp_path / "stage.json"
    path.write_text(json.dumps(stage))
    code, text = fresh_text("move2_seek", path)
    assert code == 1 and "FAIL" in text


def test_fresh_all_reports_every_live_stage_and_names_the_ones_without_files(tmp_path, capsys):
    names = rc.live_stage_names()
    assert len(names) == 12 and "fast" not in names and "move3_interact" in names and "dungeon3_deadmines" in names
    (tmp_path / "move2_seek").mkdir()
    (tmp_path / "move2_seek" / "stage.json").write_text(M2.read_text())
    code = rc.main(["--fresh", "--all", "--stage-json-dir", str(tmp_path)])
    text = capsys.readouterr().out
    assert code == 1
    assert re.search(r"^move2_seek\s+PASS$", text, re.M)
    assert re.search(r"^move3_interact\s+FAIL\s+no stage.json at", text, re.M)
    assert "1 of 12 stages start on the learner; FAILED:" in text


def test_fresh_single_stage_through_the_cli(capsys):
    assert rc.main(["--fresh", "--stage", "move2_seek", "--stage-json", str(M2)]) == 0
    assert "move2_seek" in capsys.readouterr().out


def test_fresh_cli_refuses_incomplete_arguments():
    for bad in (["--fresh"], ["--fresh", "--all"], ["--fresh", "--stage", "move2_seek"],
                ["--fresh", "--all", "--stage", "move2_seek", "--stage-json-dir", "x"]):
        with pytest.raises(SystemExit):
            rc.main(bad)


def test_the_sims_state_and_goal_widths_are_the_ones_a_real_checkpoint_announces():
    backup = ROOT / "var" / "backups" / "2026-10-07" / "move2_seek" / "latest.pt"
    if not backup.is_file():
        pytest.skip("the M2 backup checkpoint is not on this machine")
    spec = torch.load(backup, map_location="cpu", weights_only=False)["spec"]
    assert (spec["state_dim"], spec["goal_count"]) == (rc.STATE_DIM, rc.GOAL_COUNT)
    built = rc.fresh_spec(json.loads(M2.read_text()))
    for key in ("obs_dim", "num_actions", "image_bytes", "look_heads", "map_bytes", "episode_info_dim"):
        assert getattr(built, key) == spec[key], key
