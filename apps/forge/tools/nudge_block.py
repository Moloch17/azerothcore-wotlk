#!/usr/bin/env python3
"""Give a block's input columns a small starting weight in a checkpoint, so the policy begins to read them.

A block that starts fresh when a stage is seeded (the follow stage's party frames, seeded from move2_seek) has its
adapter columns zeroed: the seeded policy acts as it did and the block comes in as its columns learn. When the columns
stay near zero for tens of millions of steps (move4_follow at 40M: the no_party ablation cost nothing, decision 0034),
this draws them afresh at `--scale` times the spread of the layout's other adapter columns, in the actor's and the
critic's adapters, and writes the checkpoint back (the original kept beside it as <name>.before-nudge). Run it with
the stage cancelled, then resume.

    python3 nudge_block.py runs/move4_follow/latest.pt runs/move4_follow/stage.json party_frames --scale 0.5
"""
import argparse
import json
import shutil
from pathlib import Path

import torch


def block_columns(stage: dict, layout: str, block_name: str) -> tuple[int, int] | None:
    entry = (stage.get("layouts") or {}).get(layout) or {}
    for block in entry.get("blocks", ()):
        if block.get("name") == block_name:
            first, count = block["obs"]
            return int(first), int(count)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("checkpoint")
    parser.add_argument("stage_json")
    parser.add_argument("block")
    parser.add_argument("--scale", type=float, default=0.5,
                        help="the new columns' spread over the layout's other columns' (default 0.5)")
    parser.add_argument("--below", type=float, default=0.25,
                        help="only columns whose weights' spread is under this share of the others' are redrawn")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    path = Path(args.checkpoint)
    checkpoint = torch.load(path, map_location="cpu", weights_only=False)
    stage = json.loads(Path(args.stage_json).read_text())
    # The adapters are in the checkpoint's own layout order (its spec), which stage.json's layouts follow.
    layouts = [layout.get("name") if isinstance(layout, dict) else layout
               for layout in (checkpoint.get("spec") or {}).get("layouts") or (stage.get("layouts") or {}).keys()]
    generator = torch.Generator().manual_seed(args.seed)
    changed = 0
    for network in ("actor", "critic"):
        state = (checkpoint.get("trainer") or {}).get(network)
        if state is None:
            continue
        for index, layout in enumerate(layouts):
            key = f"adapters.{index}.weight"
            span = block_columns(stage, layout, args.block)
            if key not in state or span is None:
                continue
            first, count = span
            weight = state[key]
            columns = weight[:, first:first + count]
            others = torch.cat([weight[:, :first], weight[:, first + count:]], dim=1)
            spread = float(others.std())
            have = float(columns.std())
            print(f"{network} {layout}: {args.block} columns {first}-{first + count - 1}, spread {have:.4g} "
                  f"(others {spread:.4g})")
            if have >= args.below * spread:
                print("  already in use: left as it is")
                continue
            if not args.dry_run:
                fresh = torch.randn(columns.shape, generator=generator, dtype=weight.dtype) * (args.scale * spread)
                weight[:, first:first + count] = fresh
            changed += 1
    if args.dry_run or not changed:
        print(f"{changed} adapter blocks would change" if args.dry_run else "nothing changed")
        return 0
    backup = path.with_name(path.name + ".before-nudge")
    if not backup.exists():
        shutil.copy2(path, backup)
    torch.save(checkpoint, path)
    print(f"{changed} adapter blocks redrawn; {path} written (the original kept as {backup.name})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
