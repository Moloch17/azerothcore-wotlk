"""Evaluate a checkpoint against a running Animus Forge sim, on seeded episodes.

    python -m animus.evaluate --checkpoint runs/<name>/best.pt [--episodes 128] [--seed 1000] [--baseline random]
                              [--mask-actions NAME ...]

Uses the same seeded evaluation as training (animus.evaluation): with the same --seed and --episodes the
characters and opponents match the ones training scored. --baseline also scores the random sim policy on
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
from dataclasses import asdict, fields
from pathlib import Path

import numpy as np
import torch

from .config import REPORT_COLUMNS, TrainConfig
from .env import ForgeEnv
from .evaluation import action_mask_table, format_summary, run_evaluation
from .mappo.networks import check_image_bytes, check_look_heads, seat_sets_of, vision_of
from .mappo.trainer import MappoConfig, MappoTrainer
from .runs import resume_mismatch
from .stages import STAGE_FILE, layout_changes, load_stage
from .device import host


def mappo_from_checkpoint(saved: dict, emit=print) -> MappoConfig:
    """The MappoConfig a checkpoint was saved with, as this build knows it. Only for a checkpoint's saved config: one
    saved by an older build carries options since removed, which are dropped (one line names them). A yaml is never
    read this way: TrainConfig.load stays strict, so a typo in a live config is still an error."""
    known = {field.name for field in fields(MappoConfig)}
    dropped = sorted(key for key in saved if key not in known)
    if dropped:
        emit(f"evaluate: the checkpoint's config has {len(dropped)} option(s) this build no longer has, ignored: "
             f"{', '.join(dropped)}")
    kept = {key: value for key, value in saved.items() if key in known}
    return MappoConfig(**{**kept, "hidden": tuple(saved["hidden"])})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--episodes", type=int, default=128)
    parser.add_argument("--seed", type=int, default=1000)
    parser.add_argument("--baseline", default="",
                        help="also score the random policy (the only sim policy) on the same seeds")
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
    mappo = mappo_from_checkpoint(saved["mappo"])
    socket_path = args.socket or saved.get("socket", TrainConfig.socket)

    env = ForgeEnv(socket_path)
    spec = env.spec
    if spec.scenario != checkpoint["spec"]["scenario"]:
        raise SystemExit(f"checkpoint was trained on {checkpoint['spec']['scenario']}, sim runs {spec.scenario}")

    # Layouts must match by name as well as size: two class lists can have equally sized layouts.
    if mismatch := resume_mismatch(checkpoint["spec"], asdict(spec)):
        raise SystemExit(f"the checkpoint's {', '.join(mismatch)} do not match the sim's (AnimusForge.ClassRoles?)")
    layouts = [(layout.obs_dim, layout.num_actions) for layout in spec.layouts]

    # The checkpoint's own seat sets and camera (its stage.json), or its weights do not load.
    stage = checkpoint.get("stage")
    # And the layouts it was trained on are the sim's, block by block (a block re-laid at the same width included).
    if changes := layout_changes(stage, load_stage(saved.get("layouts_dir", TrainConfig.layouts_dir), spec.scenario)):
        raise SystemExit(f"the checkpoint's layouts are not the sim's -- {' | '.join(changes)}")
    names = [layout.name for layout in spec.layouts]
    seat_sets = seat_sets_of(stage, names) if mappo.seat_sets else None
    vision = vision_of(stage, names)
    try:
        check_image_bytes(vision, spec.image_bytes, spec.map_bytes)
        check_look_heads(vision, spec.look_heads)
    except ValueError as error:
        raise SystemExit(f"vision: {error}") from None
    trainer = MappoTrainer(layouts, spec.state_dim, mappo, seat_sets=seat_sets, vision=vision)
    trainer.load_state_dict(checkpoint["trainer"], load_optimizers=False)

    acting = trainer.acting_state(spec.num_envs, spec.agents_per_env)

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
        chosen = trainer.act(step.obs, mask, step.layout, not args.stochastic, acting, getattr(step, "image", None))[0]
        # The free look (protocol 22) goes with the actions, greedy with them unless --stochastic.
        look = trainer.wire_look(acting.look)
        return chosen if look is None else (chosen, None, look)

    try:
        env.reset()
        baseline = None
        baseline_result = None
        if args.baseline:
            baseline_result, _ = run_evaluation(env, spec, actions, args.episodes, args.seed, baseline=args.baseline,
                                                arenas=arenas)
            baseline = baseline_result.summary(REPORT_COLUMNS)
        result, _ = run_evaluation(env, spec, actions, args.episodes, args.seed, arenas=arenas,
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
