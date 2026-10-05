"""Offline tools over human capture files: `python -m animus.human <command> --capture <dir> --out <dir>`.

    summary          files, records per type, headers, dropped/paused streams and open hours
    build            human_motion_windows.npz, human_reference.json, human_trips.json, human_hard_spots.json
    fit              the executor-fit study: human_fit.json + human_fit.md
    mapper-validate  inverse mapper accuracy on emulated sequences (and casts, given --ranks and --manifest)
    spell-ranks      export spell_ranks from the forge DB container (or --sql-dump) to a CSV
    prices           proposed noise prices: human_prices.json + human_prices.md (never writes config)
    companions       per-model companion feedback: human_companions.json
    realism          a bot run's eval_motion.npz against human_reference.json: realism.json

`--from` / `--to` take `yyyy-mm-dd` or `yyyy-mm-ddThh` (UTC, inclusive). See README.md beside this file.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

from animus.human import build as build_mod
from animus.human import companions, dataset, fit, mapper, motion, prices, realism
from animus.human import reader as r
from animus.human import reference as ref
from animus.human import segment, tracks

FIT_STEP = 0.125


def _common(parser: argparse.ArgumentParser, capture: bool = True) -> None:
    parser.add_argument("--capture", required=capture, help="Animus.Capture.Dir (or a copy of it)")
    parser.add_argument("--out", required=True, help="output directory")
    parser.add_argument("--from", dest="start", default=None, help="first hour, yyyy-mm-dd[Thh] UTC")
    parser.add_argument("--to", dest="end", default=None, help="last hour, yyyy-mm-dd[Thh] UTC")


def cmd_summary(args) -> dict:
    cap = r.CaptureDir(args.capture, args.start, args.end)
    out: dict = {"hours": len(cap.hours()), "streams": {}, "index": cap.index_report(), "revisions": set()}
    for hour in cap.hours():
        for path in hour.files():
            stream, _ = r.stream_map(path)
            entry = out["streams"].setdefault(stream, {"files": 0, "bytes": 0, "records": {}, "malformed": 0,
                                                       "unknown": 0, "truncated_members": 0, "players": set()})
            entry["files"] += 1
            entry["bytes"] += path.stat().st_size
            if args.index_only:
                continue
            stats = r.FileStats(path)
            for batch in r.read_file(path, stats):
                for rtype, rows in batch.records.items():
                    name = r.PREFIX[rtype][0]
                    entry["records"][name] = entry["records"].get(name, 0) + len(rows)
                    if "player" in rows.dtype.names:
                        entry["players"].update(np.unique(rows["player"]).tolist())
            entry["malformed"] += stats.malformed
            entry["unknown"] += stats.unknown
            entry["truncated_members"] += stats.truncated_members
            if stats.header:
                out["revisions"].add((stats.header["module_revision"], stats.header["realm_build"],
                                      stats.header["format"]))
    for entry in out["streams"].values():
        entry["players"] = len(entry["players"])
    out["revisions"] = [{"module_revision": a, "realm_build": b, "format": c} for a, b, c in sorted(out["revisions"])]
    return out


def cmd_build(args) -> dict:
    result = build_mod.build(args.capture, args.out, args.start, args.end, stride=args.stride,
                             max_windows=args.max_windows, trips_per_map=args.trips_per_map, seed=args.seed)
    return {"files": {k: str(v) for k, v in result.paths.items()}, "tracks": result.tracks, "clips": result.clips,
            "windows_seen": result.windows, "trips": result.trips, "units": result.units}


def _clips(args, step: float, min_steps: int):
    """(track, clip samples, jumps per step) of human clips across the range, on `step`."""
    cap = r.CaptureDir(args.capture, args.start, args.end)
    for sessions, shard in tracks.iter_shards(cap):
        for track in tracks.build_tracks(shard.moves, shard.combat, sessions, hour=shard.hour.label, step=step):
            for clip in segment.clips(track, min_steps=min_steps):
                yield shard, track, clip


def cmd_fit(args) -> dict:
    rng = np.random.default_rng(args.seed)
    keep: list[tuple[float, np.ndarray]] = []
    need = int(round(fit.CHUNK_SECONDS / FIT_STEP)) + 1
    for _, _, clip in _clips(args, FIT_STEP, need):
        keep.append((float(rng.random()), clip.samples.copy()))
        if len(keep) > 2 * args.max_clips:
            keep.sort(key=lambda kv: kv[0])
            del keep[args.max_clips:]
    keep.sort(key=lambda kv: kv[0])
    study = fit.FitStudy(beam=args.beam, spaces=tuple(args.spaces))
    for _, samples in keep[:args.max_clips]:
        study.add_clip(samples)
    report = study.report()
    report["clips"] = min(len(keep), args.max_clips)
    js, md = fit.write(report, args.out)
    return {"json": str(js), "markdown": str(md), "clips": report["clips"],
            "expressible": {name: s["overall"].get("expressible_share") for name, s in report["spaces"].items()}}


def cmd_mapper_validate(args) -> dict:
    out: dict = {"movement": []}
    for name in ("lattice", "lattice_fine"):
        for noise in (0.0, 0.05):
            out["movement"].append(mapper.validate_movement(fit.SPACES[name], sequences=args.sequences,
                                                            length=args.length, noise_yards=noise,
                                                            noise_radians=noise / 5, seed=args.seed))
    if args.ranks and args.manifest:
        out["casts"] = mapper.validate_casts(mapper.read_spell_ranks(args.ranks),
                                             mapper.Catalog.from_manifest(args.manifest))
    path = Path(args.out) / "human_mapper_validation.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(out, indent=1))
    return {"file": str(path), "movement": [{k: m[k] for k in ("space", "noise_yards", "accuracy", "press_accuracy")}
                                            for m in out["movement"]], "casts": out.get("casts")}


def cmd_spell_ranks(args) -> dict:
    path = Path(args.out) / "spell_ranks.csv"
    rows = mapper.export_spell_ranks(path, container=args.container, env_file=args.env_file, sql_dump=args.sql_dump)
    return {"file": str(path), "rows": rows}


def cmd_prices(args) -> dict:
    units: dict[tuple, prices.UnitTally] = {}
    casts_done: set = set()
    space = fit.SPACES["lattice"]
    clips_done = 0
    for shard, track, clip in _clips(args, motion.DECISION_SECONDS, motion.WINDOW + 1):
        key = (track.player, track.hour)
        tally = units.setdefault(key, prices.UnitTally())
        if clips_done < args.max_clips:
            jumps = track.jumps[clip.start + 1:clip.end] > 0
            mapping = mapper.map_movement(clip.samples, space, beam=args.beam, jumps=jumps, confidence=False)
            prices.tally_movement(tally, clip.samples, mapping.actions, space)
            clips_done += 1
        if key not in casts_done:
            casts_done.add(key)
            req = shard.actions.get(r.CAST_REQUEST)
            mine = req[req["player"] == track.player]
            prices.tally_casts(tally, mine["ms"], mine["spell"])
    report = prices.propose(list(units.values()), budget=args.budget)
    report["clips_mapped"] = clips_done
    js, md = prices.write(report, args.out)
    return {"json": str(js), "markdown": str(md), "proposed": report["proposed"]}


def cmd_companions(args) -> dict:
    result = companions.report(args.capture, args.start, args.end)
    path = companions.write(result, args.out)
    return {"file": str(path), "models": list(result["models"])}


def cmd_realism(args) -> dict:
    reference_path = args.reference or Path(args.out) / "human_reference.json"
    reference = ref.load(reference_path)
    bot_path = Path(args.bot) if args.bot else Path(args.run) / "eval_motion.npz"
    bot = dataset.load(bot_path)
    result = ref.realism(reference, bot["windows"])
    # The headline is the number the forge's evaluations report (realism.score: contexts weighted by the bot's steps),
    # over each window's last step -- every step once, as the eval scores its tracks -- so this report and eval.csv's
    # realism_emd agree; the per-feature breakdown above is kept beside it.
    windows = np.asarray(bot["windows"])
    headline = realism.score(windows[:, -1, :], np.asarray(bot["context"]), realism.load_reference(reference_path))
    result["realism_emd"] = headline["realism_emd"]
    result["realism_emd_contexts"] = {key[len("realism_emd_"):]: value for key, value in headline.items()
                                      if key.startswith("realism_emd_")}
    result["bot"] = str(bot_path)
    path = Path(args.out) / "realism.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, indent=1, default=float))
    return {"file": str(path), "realism_emd": result["realism_emd"], "mean_emd": result["mean_emd"],
            "contexts": result["realism_emd_contexts"]}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m animus.human", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("summary", help="what a capture range holds")
    _common(p)
    p.add_argument("--index-only", action="store_true", help="sizes and index.json only, no records read")
    p.set_defaults(func=cmd_summary)

    p = sub.add_parser("build", help="dataset, reference, trips and hard spots")
    _common(p)
    p.add_argument("--stride", type=int, default=1, help="steps between window starts within a clip")
    p.add_argument("--max-windows", type=int, default=2_000_000)
    p.add_argument("--trips-per-map", type=int, default=5000)
    p.add_argument("--seed", type=int, default=0)
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("fit", help="the executor-fit study")
    _common(p)
    p.add_argument("--max-clips", type=int, default=500)
    p.add_argument("--beam", type=int, default=32)
    p.add_argument("--spaces", nargs="+", default=list(fit.SPACES), choices=list(fit.SPACES))
    p.add_argument("--seed", type=int, default=0)
    p.set_defaults(func=cmd_fit)

    p = sub.add_parser("mapper-validate", help="inverse mapper accuracy on known sequences")
    _common(p, capture=False)
    p.add_argument("--sequences", type=int, default=50)
    p.add_argument("--length", type=int, default=40)
    p.add_argument("--ranks", default=None, help="spell_ranks CSV")
    p.add_argument("--manifest", default=None, help="a layout manifest (<model>.json)")
    p.add_argument("--seed", type=int, default=0)
    p.set_defaults(func=cmd_mapper_validate)

    p = sub.add_parser("spell-ranks", help="export spell_ranks to <out>/spell_ranks.csv")
    _common(p, capture=False)
    p.add_argument("--container", default=mapper.DB_CONTAINER)
    p.add_argument("--env-file", default=None, help="an env file holding DOCKER_DB_ROOT_PASSWORD (e.g. .env)")
    p.add_argument("--sql-dump", default=None, help="parse this spell_ranks.sql instead of asking the DB")
    p.set_defaults(func=cmd_spell_ranks)

    p = sub.add_parser("prices", help="proposed noise prices (a report)")
    _common(p)
    p.add_argument("--budget", type=float, default=prices.BUDGET, help="reward a median human may pay a minute")
    p.add_argument("--max-clips", type=int, default=2000)
    p.add_argument("--beam", type=int, default=16)
    p.set_defaults(func=cmd_prices)

    p = sub.add_parser("companions", help="per-model companion feedback")
    _common(p)
    p.set_defaults(func=cmd_companions)

    p = sub.add_parser("realism", help="a bot run's motion against the human reference")
    _common(p, capture=False)
    p.add_argument("--run", default=None, help="a run directory holding eval_motion.npz")
    p.add_argument("--bot", default=None, help="an eval_motion.npz (instead of --run)")
    p.add_argument("--reference", default=None, help="human_reference.json (default <out>/human_reference.json)")
    p.set_defaults(func=cmd_realism)

    args = parser.parse_args(argv)
    if args.command == "realism" and not (args.run or args.bot):
        parser.error("realism needs --run or --bot")
    result = args.func(args)
    json.dump(result, sys.stdout, indent=1, default=str)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
