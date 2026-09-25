#!/usr/bin/env python3
"""Move training run directories to the current stage names (the 2026-09 renumberings).

A run directory is a stage's checkpoints: a stage renamed without its directory looks like a stage that has never
trained, and every stage seeding from it starts from nothing after one quiet log line. So the rename of the stages
and the move of their runs are one change, and this is the half that touches disk.

    tools/rename_runs.py <animus-forge dir> [--apply]

Walks every `runs/` under it (`shared/runs`, `druid/runs`, `bench/runs`, ...), including `_archive/<name>-<date>`
and `_finetune/<name>`, and prints what would move; `--apply` moves it. Idempotent: a directory already at its new
name is left alone, and one whose new name is taken is reported and skipped. Runs of stages that were folded away
(`stage1d_glide`, `stage1f_breathe`) or dropped (`stage9_pvp`, `mix_duel_pvp`) go to `_archive/` with a date.
"""

import datetime as dt
import re
import sys
from pathlib import Path

TABLE = {
    "stage1b_indoor": "stage2_indoor",
    "stage1c_jump": "stage3_jump",
    "stage1e_dive": "stage4_dive",
    "stage2_dodge": "stage5_dodge",
    "stage3_travel": "stage6_travel",
    "stage4_flight": "stage7_flight",
    "stage5_duel": "stage8_duel",
    "stage6_pack": "stage9_pack",
    "stage7_gauntlet": "stage10_gauntlet",
    "stage8_endurance": "stage11_endurance",
    "stage13_arena": "stage12_pvp",
    "stage10_evade": "stage13_evade",
    "stage11_hide": "stage14_hide",
    "stage12_stealth": "stage15_stealth",
    "stage14_companion": "stage16_companion",
    "stage15_party": "stage17_party",
    "stage16_tanking": "stage18_tanking",
    "stage17_triage": "stage19_triage",
    "stage18_flag": "stage20_flag",
    "stage19_warsong": "stage25_warsong",
    "stage20_raid_single": "stage28_raid_single",
    "stage21_raid_gauntlet": "stage29_raid_gauntlet",
    # The second renumber (2026-09-24): the objective stages and the raids moved up to leave 20-23 and 30-32 for the
    # life stages, the dungeon and the real raids. A directory at either generation's name lands at the final one.
    "stage20_flag": "stage24_flag",
    "stage21_warsong": "stage25_warsong",
    "stage22_duo_led": "stage26_duo_led",
    "stage23_crossroads": "stage27_crossroads",
    "stage24_raid_single": "stage28_raid_single",
    "stage25_raid_gauntlet": "stage29_raid_gauntlet",
}
# The first-generation names of the same stages resolve to the final names too.
TABLE["stage18_flag"] = "stage24_flag"
TABLE["stage22_duo_led"] = "stage26_duo_led"
TABLE["stage23_crossroads"] = "stage27_crossroads"
GONE = {"stage1d_glide", "stage1f_breathe", "stage9_pvp", "mix_duel_pvp"}


def moves(root: Path):
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    for runs in sorted(root.rglob("runs")):
        if not runs.is_dir():
            continue
        for entry in sorted(runs.iterdir()):
            if not entry.is_dir() or entry.name.startswith("_"):
                continue
            if entry.name in TABLE:
                yield entry, runs / TABLE[entry.name]
            elif entry.name in GONE:
                yield entry, runs / "_archive" / f"{entry.name}-{stamp}"
        for sub in ("_archive", "_finetune"):
            if not (runs / sub).is_dir():
                continue
            for entry in sorted((runs / sub).iterdir()):
                if not entry.is_dir():
                    continue
                stem, dash, date = entry.name.partition("-")
                if stem in TABLE:
                    yield entry, runs / sub / f"{TABLE[stem]}{dash}{date}"


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    root = Path(argv[1])
    apply = "--apply" in argv
    count = 0
    # Highest source number first within a runs directory: stage25_raid_gauntlet -> 29 before stage21_warsong -> 25,
    # so a destination is free when its move comes.
    def number(path: Path) -> int:
        m = re.match(r"stage(\d+)", path.name)
        return int(m.group(1)) if m else -1

    for src, dst in sorted(moves(root), key=lambda move: (str(move[0].parent), -number(move[0]))):
        if dst.exists():
            print(f"skip  {src} -> {dst} (exists)")
            continue
        print(f"{'move' if apply else 'would'}  {src} -> {dst}")
        if apply:
            dst.parent.mkdir(parents=True, exist_ok=True)
            src.rename(dst)
        count += 1
    print(f"{count} {'moved' if apply else 'to move'}" + ("" if apply else " (add --apply)"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
