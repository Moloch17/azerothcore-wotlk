"""One pass over a capture range -> the four files training uses (`python -m animus.human build`).

Shard by shard (one hour of one map, tracks.iter_shards): human tracks (companions left out), their trips and
clips (segment), windows into the dataset, steps into the reference histograms, per-unit movement counts, and
per-player action metrics, fights, deaths, falls and stuck spots. Units -- one player in one hour shard -- are
finished at the end of their shard, so memory holds one shard's records and the accumulators' bounded samples.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from animus.human import motion
from animus.human import reader as r
from animus.human import reference as ref
from animus.human import segment
from animus.human.dataset import DatasetBuilder
from animus.human.hard_spots import HardSpots
from animus.human.tracks import Shard, SessionTable, Track, build_tracks, iter_shards
from animus.human.trips import TripPool


@dataclass
class BuildResult:
    paths: dict[str, Path] = field(default_factory=dict)
    tracks: int = 0
    clips: int = 0
    windows: int = 0
    trips: int = 0
    units: int = 0


def source_of(capture: str | Path, start: str | None, end: str | None) -> dict:
    return {"capture_dir": str(capture), "from": start, "to": end}


def shard_tracks(sessions: SessionTable, shard: Shard, include_companions: bool = False) -> list[Track]:
    return build_tracks(shard.moves, shard.combat, sessions, hour=shard.hour.label,
                        include_companions=include_companions)


def build(capture: str | Path, out: str | Path, start: str | None = None, end: str | None = None,
          stride: int = 1, max_windows: int = 2_000_000, trips_per_map: int = 5000, seed: int = 0) -> BuildResult:
    cap = r.CaptureDir(capture, start, end)
    source = source_of(capture, start, end)
    data = DatasetBuilder(stride=stride, max_windows=max_windows, seed=seed)
    reference = ref.ReferenceBuilder()
    pool = TripPool(per_map=trips_per_map, seed=seed)
    spots = HardSpots()
    result = BuildResult()
    for sessions, shard in iter_shards(cap):
        reference.hours.add(shard.hour.label)
        tracks = shard_tracks(sessions, shard)
        result.tracks += len(tracks)
        interacts = shard.actions.get(r.INTERACT)
        counts: dict[int, dict] = {}
        for track in tracks:
            reference.players.add(track.player)
            trips = segment.trips(track, interacts)
            for trip in trips:
                pool.add(trip)
            result.trips += len(trips)
            for clip in segment.clips(track, trips=trips):
                result.clips += 1
                result.windows += data.add_clip(clip.samples, track.latency_ms)
                feats = motion.features(clip.samples)
                reference.add_steps(feats, motion.step_contexts(clip.samples))
                ref.add_counts(counts.setdefault(track.player, {}),
                               ref.movement_counts(feats, track.jumps[clip.start + 1:clip.end]))
            for a, b in segment.stuck_spans(track):
                spots.add(track.map, "stuck", track.samples[a, motion.X:motion.Z + 1])
        humans = {t.player: t.info for t in tracks}
        acting = set(np.unique(shard.actions.get(r.CAST_REQUEST)["player"]).tolist())
        for player in set(humans) | {p for p in acting if not sessions.is_companion(int(p))}:
            player = int(player)
            info = humans.get(player) or sessions.info(player)
            metrics = ref.movement_metrics(counts.get(player, {}))
            metrics.update(ref.action_metrics(shard.actions, shard.outcomes, player,
                                              segment.fights(shard.outcomes, player)))
            if metrics:
                result.units += 1
                reference.add_unit(info, metrics)
        deaths = [d for d in segment.deaths(shard.outcomes, shard.moves) if not sessions.is_companion(d["player"])]
        spots.add_deaths(deaths)
        spots.add_falls(shard.moves.get(r.MOVE), sessions.companions)
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    result.paths["dataset"] = data.write(out, dict(source, players=len(reference.players),
                                                   hours=len(reference.hours)))
    path = out / "human_reference.json"
    path.write_text(json.dumps(reference.result(source)))
    result.paths["reference"] = path
    full = dict(source, players=len(reference.players), hours=len(reference.hours))
    result.paths["trips"] = pool.write(out, full)
    result.paths["hard_spots"] = spots.write(out, full)
    return result
