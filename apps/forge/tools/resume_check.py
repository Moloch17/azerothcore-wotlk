#!/usr/bin/env python3
"""A dry run of `forge resume <stage>` on CPU, without the sim: will this checkpoint resume on this build?

    resume_check.py [RUN_DIR] --stage-json STAGE_JSON [--config YAML] [--spec SPEC_JSON] [--set KEY=VALUE ...]
                    [--overlay YAML ...] [--checkpoint PT]

RUN_DIR is the run directory (runs/<stage>; default runs/<current directory's stage>), STAGE_JSON the stage.json the NEW
build wrote for the stage (docs/forge/deploy-gate.md, step 4). The checkpoint is RUN_DIR/latest.pt unless --checkpoint
names another (copy a live run's files out first; this tool only reads, and never writes into RUN_DIR).

It does what the learner does at `--resume` (animus.train.TrainingRun.__init__ and _load_or_seed), with the same
functions, in the same order:
  1. loads the stage's learner config as the sim launches it: TrainConfig.load(configs/<stage>.yaml, --set, --overlay);
  2. builds the networks for the new stage.json on CPU (train.trainer_inputs and train.make_trainer);
  3. the resume compatibility check (runs.resume_mismatch: scenario, agents_per_env, obs_dim, state_dim, num_actions,
     layouts) and the layout check (stages.layout_changes: block spans, revisions, widths) -- both must be empty;
  4. loads the checkpoint's weights (MappoTrainer.load_state_dict), reporting every missing and unexpected key;
  5. the saved evaluation, fade and cost-ladder state into the stage's own objects (stage.restore_evaluation_state):
     rung, falls, held, settled;
  6. the config values that differ from the ones saved in the checkpoint.

What it cannot do without the sim: the SPEC the sim sends (num_envs, agents_per_env, state_dim and the camera's byte
counts are not in stage.json: they are taken from the checkpoint's saved spec unless --spec gives the new build's
spec.json, which the learner writes to a run directory on any start), and one rollout step (it needs observations from
the sim). Both are said in the output, as UNVERIFIED, rather than faked.

Exit status: 0 every check passed (UNVERIFIED lines allowed), 1 a check failed, 2 the inputs could not be read.
Run it in the dev container with the learner's python: /azerothcore/apps/forge/python/.venv/bin/python.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path

PYTHON_DIR = Path(__file__).resolve().parents[1] / "python"
sys.path.insert(0, str(PYTHON_DIR))

#: Config values the sim hands the learner at launch (LearnerProcess.cpp, LearnerArgs): a resume on a machine, or the
#: gate's dry run here, differs on them without anything being wrong.
SIM_INJECTED = ("socket", "run_name", "runs_dir", "layouts_dir", "torch_threads", "cluster_sims", "rank", "ranks",
                "local_rank", "local_ranks", "dist_address", "dist_iface", "train_device", "rollout_device",
                "mappo.rank_sync")


@dataclass
class Check:
    status: str          # PASS, FAIL, UNVERIFIED or INFO
    title: str
    details: list[str] = field(default_factory=list)


@dataclass
class Report:
    checks: list[Check] = field(default_factory=list)

    def add(self, status: str, title: str, *details: str) -> Check:
        check = Check(status, title, list(details))
        self.checks.append(check)
        return check

    @property
    def failed(self) -> bool:
        return any(check.status == "FAIL" for check in self.checks)

    def text(self) -> str:
        lines = []
        for check in self.checks:
            lines.append(f"[{check.status}] {check.title}")
            lines += [f"    {detail}" for detail in check.details]
        verdict = "NOT OK: a check failed" if self.failed else "OK: every check that can run here passed"
        unverified = [check.title for check in self.checks if check.status == "UNVERIFIED"]
        lines.append(verdict + (f" ({len(unverified)} unverified without the sim)" if unverified else ""))
        return "\n".join(lines)


def flatten(value, prefix: str = "") -> dict:
    """A nested dict as {"a.b": leaf}; lists and scalars are leaves."""
    if isinstance(value, dict):
        out = {}
        for key, item in value.items():
            out.update(flatten(item, f"{prefix}{key}."))
        return out
    return {prefix[:-1]: value}


def config_differences(saved: dict, now: dict) -> list[tuple[str, object, object]]:
    """[(key, saved, now)] for the config values that differ (a key only one side has shows "<absent>")."""
    saved, now = flatten(saved), flatten(now)
    missing = "<absent>"
    return [(key, saved.get(key, missing), now.get(key, missing))
            for key in sorted(saved.keys() | now.keys()) if saved.get(key, missing) != now.get(key, missing)]


def build_spec(checkpoint_spec: dict, stage: dict, spec_json: dict | None):
    """The Spec the new build would send, and what of it could not be derived.

    With the new build's own spec.json that is all of it. Without, the checkpoint's saved spec stands for what
    stage.json does not carry (agents_per_env, state_dim, env counts, the camera's bytes) and stage.json overrides what
    it does: the scenario, each layout's obs_dim and num_actions (the layouts in the checkpoint's order, any new one
    after them), their maxima, and the episode info columns."""
    from animus.bench_learner import spec_from_dict

    if spec_json is not None:
        return spec_from_dict(spec_json), []
    raw = dict(checkpoint_spec)
    layouts = stage.get("layouts", {})
    names = [item["name"] for item in raw.get("layouts", ())]
    names += [name for name in layouts if name not in names]
    raw["layouts"] = [{"name": name, "obs_dim": layouts[name]["obs_dim"], "num_actions": layouts[name]["num_actions"]}
                      for name in names if name in layouts]
    raw["scenario"] = stage.get("stage", raw.get("scenario"))
    raw["obs_dim"] = max(item["obs_dim"] for item in raw["layouts"])
    raw["num_actions"] = max(item["num_actions"] for item in raw["layouts"])
    raw["episode_info_names"] = list(stage.get("episode_info", ()))
    raw["episode_info_dim"] = len(raw["episode_info_names"])
    return spec_from_dict(raw), ["agents_per_env", "state_dim", "image_bytes", "look_heads", "map_bytes"]


def state_keys(module, state: dict) -> tuple[list[str], list[str], list[str]]:
    """(missing, unexpected, tolerated) of `state` against `module`, as the trainer loads it: the blind-column masks
    are never taken from a checkpoint and an actor from before the goal scale loads it at zero, so those are
    tolerated and listed apart."""
    from animus.mappo.networks import BLIND_KEEP_PREFIXES, _GOAL_SCALE_KEYS, without_blind_columns

    have = set(module.state_dict())
    given = set(without_blind_columns(state))
    missing = sorted(have - given)
    tolerated = [key for key in missing if key in _GOAL_SCALE_KEYS or key.split(".")[-1].startswith(BLIND_KEEP_PREFIXES)]
    return [key for key in missing if key not in tolerated], sorted(given - have), tolerated


def fmt(value) -> str:
    text = json.dumps(value, sort_keys=True, default=str)
    return text if len(text) <= 70 else text[:67] + "..."


def check_resume(run_dir: Path, stage_json: Path, config_path: Path, checkpoint_path: Path | None = None,
                 spec_json: Path | None = None, sets: list[str] | None = None,
                 overlays: list[str] | None = None) -> Report:
    import torch

    from animus.config import TrainConfig
    from animus.runs import resume_checkpoint_path, resume_mismatch
    from animus.stage import ConvergenceController, restore_evaluation_state
    from animus.stages import layout_changes
    from animus.train import make_trainer, trainer_inputs

    report = Report()
    stage = json.loads(stage_json.read_text())
    path = checkpoint_path or resume_checkpoint_path(run_dir)
    checkpoint = torch.load(path, map_location="cpu", weights_only=False)
    report.add("INFO", f"checkpoint {path}",
               f"update {checkpoint.get('update')}, env_steps {checkpoint.get('env_steps'):,}, saved with scenario "
               f"{checkpoint.get('spec', {}).get('scenario')}",
               f"new stage.json: {stage_json} ({stage.get('stage')}, format {stage.get('format')})")

    # 1. The config, as the sim launches the learner for this stage.
    config = TrainConfig.load(config_path, sets or [], overlays or [])
    config.run_name = stage.get("stage", run_dir.name)
    report.add("PASS", f"learner config loaded: {config_path}",
               f"run_name {config.run_name}, eval.every_env_steps {config.eval.every_env_steps:,}, "
               f"fade {'on' if config.fade.enabled else 'off'}, costs {'on' if config.costs.enabled else 'off'}")

    # 2. The spec the new build would send.
    spec, derived_from_checkpoint = build_spec(checkpoint.get("spec", {}), stage,
                                               json.loads(spec_json.read_text()) if spec_json else None)
    if derived_from_checkpoint:
        report.add("UNVERIFIED", "the sim's SPEC (stage.json does not carry it)",
                   f"taken from the checkpoint: {', '.join(derived_from_checkpoint)}; pass --spec <the new build's "
                   "spec.json> to check them")

    # 3. The compatibility checks. Both must be empty.
    mismatch = resume_mismatch(checkpoint.get("spec", {}), dataclasses.asdict(spec))
    report.add("FAIL" if mismatch else "PASS", "resume compatibility (runs.resume_mismatch)",
               *([f"changed since the checkpoint: {', '.join(mismatch)}"] if mismatch else ["empty"]))
    saved_stage = checkpoint.get("stage")
    if saved_stage is None:
        report.add("UNVERIFIED", "layout check (stages.layout_changes)",
                   "the checkpoint carries no stage.json, so block spans and revisions cannot be compared")
    else:
        changes = layout_changes(saved_stage, stage)
        report.add("FAIL" if changes else "PASS", "layout check (stages.layout_changes)",
                   *(changes or ["empty: every layout's blocks, spans and revisions are as saved"]))

    # 4. The networks for the new stage.json, and the weights into them.
    try:
        inputs = trainer_inputs(config, spec, stage)
        trainer = make_trainer(config, spec, inputs, device="cpu")
    except (SystemExit, ValueError, KeyError) as error:
        report.add("FAIL", "build the networks for the new stage.json on CPU", str(error))
        return report
    parameters = sum(parameter.numel() for parameter in trainer.actor.parameters())
    report.add("PASS", "networks built for the new stage.json on CPU",
               f"{len(spec.layouts)} layouts, actor {parameters:,} parameters, camera "
               f"{'yes' if inputs.vision is not None else 'no'}, seat sets {'yes' if inputs.seat_sets else 'no'}")
    state = checkpoint["trainer"]
    problems = []
    for name, module, saved in (("actor", trainer.actor, state["actor"]), ("critic", trainer.critic, state["critic"])):
        missing, unexpected, tolerated = state_keys(module, saved)
        status = "FAIL" if missing or unexpected else "PASS"
        shapes = [key for key, tensor in module.state_dict().items()
                  if key in saved and tuple(saved[key].shape) != tuple(tensor.shape)]
        if shapes:
            status = "FAIL"
        problems.append(status == "FAIL")
        report.add(status, f"{name} state: {len(module.state_dict())} keys in the network, {len(saved)} in the checkpoint",
                   f"missing {len(missing)}: {', '.join(missing[:10]) or '-'}",
                   f"unexpected {len(unexpected)}: {', '.join(unexpected[:10]) or '-'}",
                   f"wrong shape {len(shapes)}: {', '.join(shapes[:10]) or '-'}",
                   f"tolerated by the loader (blind-column masks, goal scale): {len(tolerated)}")
    try:
        trainer.load_state_dict(state)
        report.add("PASS", "MappoTrainer.load_state_dict (the call the resume makes, optimizers included)")
        trainer.set_goal_space(stage, [layout.name for layout in spec.layouts])
        clear = trainer.director_columns_clear()
        report.add("PASS" if clear else "FAIL", "director/seat-set/camera blind columns still read nothing (resume's guard)")
    except (RuntimeError, ValueError, KeyError) as error:
        report.add("FAIL", "MappoTrainer.load_state_dict", str(error).splitlines()[0][:400])

    # 5. The saved evaluation and ladder state, into the stage's own objects.
    wanted = config.eval.score_column()
    score_kind = wanted if wanted in spec.episode_info_names else ""
    controller = ConvergenceController(
        config, [layout.name for layout in spec.layouts],
        sim_fallback_ceiling=float((stage.get("tuning") or {}).get("Markers.FallbackCeiling", 0.0)))
    try:
        dropped = restore_evaluation_state(controller.tracker, controller, checkpoint, score_kind)
    except Exception as error:  # noqa: BLE001 - whatever the state is, say what it did
        report.add("FAIL", "restore_evaluation_state", f"{type(error).__name__}: {error}")
        return report
    saved_controller = checkpoint.get("controller") or {}
    for name, ladder in (("fade", controller.fade), ("costs", controller.costs)):
        saved_rung = (saved_controller.get(name) or {}).get("rung")
        clamped = saved_rung is not None and saved_rung != ladder.rung
        report.add("INFO" if ladder.enabled else "INFO", f"{name} ladder ({'enabled' if ladder.enabled else 'disabled in the config'})",
                   f"rung {ladder.rung} of {len(ladder.rungs)} (scale x{ladder.scale:g}); saved rung {saved_rung}"
                   + (" -- CLAMPED to the configured rungs" if clamped else ""),
                   f"falls {dict(ladder.falls)}; held {ladder.held}; settled {ladder.settled}; "
                   f"evaluations at the rung {ladder.evals_at_rung}; gate {ladder.gate_metric or '-'} "
                   f">= {ladder.gate_value:g}")
    report.add("PASS" if not dropped else "FAIL", "evaluation state (best score, convergence history)",
               *([f"kept: score kind {score_kind or 'the return'}, best {controller.tracker.best}, "
                  f"evaluations {controller.evals}"] if not dropped else [dropped]))

    # 6. Config values that differ from the checkpoint's.
    differences = config_differences(checkpoint.get("config", {}), config.to_dict())
    expected = [item for item in differences if item[0] in SIM_INJECTED]
    real = [item for item in differences if item[0] not in SIM_INJECTED]
    report.add("INFO", f"config values that differ from the checkpoint's: {len(real)} of the stage's, "
                       f"{len(expected)} the sim sets at launch",
               *[f"{key}: saved {fmt(old)} -> now {fmt(new)}" for key, old, new in real[:60]],
               *([f"... and {len(real) - 60} more"] if len(real) > 60 else []),
               *([f"(sim-injected, expected: {', '.join(item[0] for item in expected)})"] if expected else []))

    report.add("UNVERIFIED", "one rollout step",
               "needs observations from the sim: start the stage (docs/forge/deploy-gate.md, step 8) and watch the "
               "first updates")
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run_dir", nargs="?", type=Path, help="runs/<stage> (a copy of it; only read)")
    parser.add_argument("--stage-json", type=Path, required=True, help="the stage.json the new build wrote")
    parser.add_argument("--config", type=Path, help="the stage's learner yaml (default configs/<stage>.yaml)")
    parser.add_argument("--checkpoint", type=Path, help="the checkpoint (default RUN_DIR/latest.pt)")
    parser.add_argument("--spec", type=Path, help="the new build's spec.json")
    parser.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", dest="sets")
    parser.add_argument("--overlay", action="append", default=[], metavar="YAML")
    args = parser.parse_args(argv)
    try:
        stage_name = json.loads(args.stage_json.read_text()).get("stage")
        run_dir = args.run_dir or Path("runs") / str(stage_name)
        config = args.config or PYTHON_DIR / "configs" / f"{stage_name}.yaml"
        report = check_resume(run_dir, args.stage_json, config, args.checkpoint, args.spec, args.sets, args.overlay)
    except (OSError, ValueError, FileNotFoundError) as error:
        print(f"resume_check: {error}", file=sys.stderr)
        return 2
    print(report.text())
    return 1 if report.failed else 0


if __name__ == "__main__":
    sys.exit(main())
