"""Evaluate a checkpoint against a running Animus Forge sim, on seeded episodes.

    python -m animus.evaluate --checkpoint runs/<name>/best.pt [--episodes 128] [--seed 1000] [--baseline fight]
                              [--mask-actions NAME ...]

Uses the same seeded evaluation as training (animus.evaluation): with the same --seed and --episodes the
characters and opponents match the ones training scored. --baseline also scores a scripted sim policy on
those seeds. The sim must run the checkpoint's scenario with AnimusForge.Policy = "remote" and no learner of
its own attached (AnimusForge.Learner.AutoStart = 0).

--mask-actions scores the checkpoint with those actions forbidden, resolved by name per layout as training's
eval.mask_actions is (animus.evaluation.action_mask_table): what a policy arrives at without the actions it leaned
on. A name no layout of the checkpoint's stage has is refused, so a mask meant for one build cannot silently
forbid nothing on another.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict
from pathlib import Path

import numpy as np
import torch

from .bootstrap import DIRECTOR_LAYOUT
from .config import REPORT_COLUMNS, TrainConfig
from .env import ForgeEnv
from .evaluation import action_mask_table, format_summary, run_evaluation
from .mappo.networks import check_image_bytes, seat_sets_of, vision_of
from .mappo.trainer import MappoConfig, MappoTrainer
from .runs import resume_mismatch
from .stages import STAGE_FILE, layout_changes, load_stage
from .device import host


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--episodes", type=int, default=128)
    parser.add_argument("--seed", type=int, default=1000)
    parser.add_argument("--baseline", default="", help="also score this scripted sim policy on the same seeds")
    parser.add_argument("--opponent-baseline", action="store_true",
                        help="self-play arenas: the baseline plays the other side (needs --baseline)")
    parser.add_argument("--socket", help="override the socket stored in the checkpoint config")
    parser.add_argument("--stochastic", action="store_true", help="sample actions instead of taking the argmax")
    parser.add_argument("--episodes-file", metavar="PATH",
                        help="write one JSON line per scored episode (seed, layout, return, episode info) there, as "
                             "training writes eval_episodes.jsonl: which seeds a class fails, not just its mean")
    parser.add_argument("--mask-actions", nargs="*", metavar="NAME",
                        help="forbid these actions (by name, resolved per layout) while scoring; default: the "
                             "checkpoint's own eval.mask_actions")
    args = parser.parse_args()

    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    saved = checkpoint["config"]
    mappo = MappoConfig(**{**saved["mappo"], "hidden": tuple(saved["mappo"]["hidden"])})
    socket_path = args.socket or saved.get("socket", TrainConfig.socket)

    env = ForgeEnv(socket_path)
    spec = env.spec
    if spec.scenario != checkpoint["spec"]["scenario"]:
        raise SystemExit(f"checkpoint was trained on {checkpoint['spec']['scenario']}, sim runs {spec.scenario}")

    # Layouts must match by name as well as size: two class lists can have equally sized layouts.
    if mismatch := resume_mismatch(checkpoint["spec"], asdict(spec)):
        raise SystemExit(f"the checkpoint's {', '.join(mismatch)} do not match the sim's (AnimusForge.ClassRoles?)")
    layouts = [(layout.obs_dim, layout.num_actions) for layout in spec.layouts]

    # The checkpoint's own director and seat sets (its stage.json), or its weights do not load.
    stage = checkpoint.get("stage")
    # And the layouts it was trained on are the sim's, block by block (a block re-laid at the same width included).
    if changes := layout_changes(stage, load_stage(saved.get("layouts_dir", TrainConfig.layouts_dir), spec.scenario)):
        raise SystemExit(f"the checkpoint's layouts are not the sim's -- {' | '.join(changes)}")
    names = [layout.name for layout in spec.layouts]
    director = ((names.index(DIRECTOR_LAYOUT), stage["director"])
                if stage and "director" in stage and DIRECTOR_LAYOUT in names else None)
    seat_sets = seat_sets_of(stage, names) if mappo.seat_sets else None
    vision = vision_of(stage, names)
    try:
        check_image_bytes(vision, spec.image_bytes)
    except ValueError as error:
        raise SystemExit(f"vision: {error}") from None
    trainer = MappoTrainer(layouts, spec.state_dim, mappo, director=director, seat_sets=seat_sets, vision=vision)
    trainer.load_state_dict(checkpoint["trainer"], load_optimizers=False)

    acting = trainer.acting_state(spec.num_envs, spec.agents_per_env)

    if args.opponent_baseline and not args.baseline:
        raise SystemExit("--opponent-baseline needs --baseline")
    opponents = args.baseline if args.opponent_baseline else ""
    stage = checkpoint.get("stage") or {}
    if not stage and (stage_path := Path(args.checkpoint).parent / STAGE_FILE).is_file():
        # An older checkpoint carries no stage; the run directory's stage.json is the one it was trained on.
        stage = json.loads(stage_path.read_text())
    arenas = tuple(arena["name"] for arena in stage.get("arenas", ()))
    action_names = {name: layout.get("action_names", []) for name, layout in stage.get("layouts", {}).items()}

    masked = tuple(args.mask_actions if args.mask_actions is not None else saved.get("eval", {}).get("mask_actions", ()))
    try:
        forbidden = action_mask_table(masked, [layout.name for layout in spec.layouts], action_names, spec.num_actions)
    except ValueError as error:
        raise SystemExit(str(error)) from None

    def actions(step):
        acting.clear(step.done)
        # The sim's mask less the actions this scoring may not take, per layout.
        mask = host(step.mask) if forbidden is None else np.logical_and(host(step.mask), ~forbidden[step.layout])
        return trainer.act(step.obs, mask, step.layout, not args.stochastic, acting, getattr(step, "image", None))[0]

    try:
        env.reset()
        baseline = None
        baseline_result = None
        if args.baseline:
            baseline_result, _ = run_evaluation(env, spec, actions, args.episodes, args.seed, baseline=args.baseline,
                                                opponents=opponents, arenas=arenas)
            baseline = baseline_result.summary(REPORT_COLUMNS)
        result, _ = run_evaluation(env, spec, actions, args.episodes, args.seed, opponents=opponents, arenas=arenas,
                                   action_names=action_names)
    finally:
        env.close()

    if args.episodes_file:
        with open(args.episodes_file, "w") as f:
            for episodes in ([baseline_result] if baseline else []) + [result]:
                for row in episodes.episodes_log(REPORT_COLUMNS):
                    f.write(json.dumps(row) + "\n")
        print(f"Wrote {args.episodes_file}")

    print(f"{spec.scenario} (update {checkpoint['update']}): score {result.score:.4g} over {result.episodes} seeded "
          f"episodes, {'stochastic' if args.stochastic else 'greedy'} policy [learner/{args.baseline or '-'}]"
          + (f", without {', '.join(masked)}" if masked else ""))
    print(format_summary(result.summary(REPORT_COLUMNS), baseline, REPORT_COLUMNS))


if __name__ == "__main__":
    main()
