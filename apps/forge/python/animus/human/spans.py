"""The visits of map instances (FORMAT.md §2.2): who was on which (map, instance) when, read off the session
stream alone. A **span** is one visit of one map instance by one or more players whose visits overlap or touch: a
two-player Deadmines run is one span, with both players, from the first one in to the last one out. `dungeon_run`
takes a span and reads the map's files party-wide for it.

A player's SessionStart and SessionContext rows carry their (map, instance) from each row's `ms` to the next row's
with a different pair, or to their SessionEnd; a visit still open at the end of the range (no later row, no end) is
`open`. Format 4 files have no `instance` (it reads 0), so their visits of one map all fall into instance 0.
"""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from animus.human import reader as r

JOIN_MS = 5 * 60 * 1000     # visits of one instance this close (a wipe's corpse run, a relog) are one span


@dataclass
class Visit:
    player: int
    kind: int               # 0 human, 1 companion
    map: int
    instance: int
    start_ms: int
    end_ms: int | None      # None: still on it at the end of the range


@dataclass
class Span:
    map: int
    instance: int
    start_ms: int
    end_ms: int | None
    players: list[int] = field(default_factory=list)       # humans
    companions: list[int] = field(default_factory=list)
    visits: list[Visit] = field(default_factory=list)
    index: int = 0

    @property
    def open(self) -> bool:
        return self.end_ms is None

    @property
    def seconds(self) -> float | None:
        return None if self.end_ms is None else (self.end_ms - self.start_ms) / 1000.0

    @property
    def label(self) -> str:
        return f"{self.map}-{self.instance}-{iso(self.start_ms)}"

    def window(self) -> tuple[int, int]:
        """[start, end] in ms, the end far off for an open span."""
        return self.start_ms, self.end_ms if self.end_ms is not None else self.start_ms + 7 * 24 * 3600 * 1000

    def covers(self, ms: int) -> bool:
        lo, hi = self.window()
        return lo <= ms <= hi

    def hours(self, cap: r.CaptureDir) -> list[r.HourDir]:
        """The hour directories the span touches (an open one: every hour from its start)."""
        lo, hi = self.window()
        out = []
        for hour in cap.hours():
            hour_ms = int(hour.hour.replace(tzinfo=dt.timezone.utc).timestamp() * 1000)
            if hour_ms + 3600 * 1000 < lo or hour_ms > hi:
                continue
            out.append(hour)
        return out

    def describe(self) -> dict:
        return {"index": self.index, "map": self.map, "instance": self.instance, "start_ms": self.start_ms,
                "end_ms": self.end_ms, "start_utc": iso(self.start_ms),
                "end_utc": iso(self.end_ms) if self.end_ms is not None else None, "seconds": self.seconds,
                "open": self.open, "players": list(self.players), "companions": list(self.companions),
                "visits": len(self.visits)}


def iso(ms: int) -> str:
    return dt.datetime.fromtimestamp(ms / 1000.0, dt.timezone.utc).strftime("%Y-%m-%dT%H%M%SZ")


def _session_rows(cap: r.CaptureDir) -> np.ndarray:
    """Every SessionStart, SessionContext and SessionEnd row of the range as one array (ms, player, kind, map,
    instance, end), sorted by player then time."""
    parts = []
    for _, _, batch in cap.iter_stream("session"):
        for rtype in (r.SESSION_START, r.SESSION_CONTEXT):
            rows = batch.get(rtype)
            if len(rows):
                part = np.zeros(len(rows), ROW)
                for name in ("ms", "player", "kind", "map", "instance"):
                    part[name] = rows[name]
                parts.append(part)
        ends = batch.get(r.SESSION_END)
        if len(ends):
            part = np.zeros(len(ends), ROW)
            for name in ("ms", "player", "map", "instance"):
                part[name] = ends[name]
            part["end"] = 1
            parts.append(part)
    if not parts:
        return np.zeros(0, ROW)
    rows = np.concatenate(parts)
    return rows[np.lexsort((rows["end"], rows["ms"], rows["player"]))]


ROW = np.dtype([("ms", "<u8"), ("player", "<u8"), ("kind", "u1"), ("map", "<u4"), ("instance", "<u4"),
                ("end", "u1")])


def visits(cap: r.CaptureDir) -> list[Visit]:
    """Every player's visits in the range, in time order."""
    rows = _session_rows(cap)
    out: list[Visit] = []
    current: Visit | None = None
    kind = 0
    last_player = None
    for row in rows:
        player = int(row["player"])
        if player != last_player:
            if current is not None:
                out.append(current)
            current = None
            last_player = player
        ms = int(row["ms"])
        if int(row["end"]):
            if current is not None:
                current.end_ms = ms
                out.append(current)
                current = None
            continue
        kind = int(row["kind"])
        pair = (int(row["map"]), int(row["instance"]))
        if current is not None and (current.map, current.instance) == pair:
            continue
        if current is not None:
            current.end_ms = ms
            out.append(current)
        current = Visit(player, kind, pair[0], pair[1], ms, None)
    if current is not None:
        out.append(current)
    out.sort(key=lambda v: (v.start_ms, v.player))
    return out


def list_spans(capture_dir: str | Path | r.CaptureDir, date: str | None = None, start: str | None = None,
               end: str | None = None, maps: set[int] | None = None) -> list[Span]:
    """The spans of the range (a `date` is one whole day, `start`/`end` an hour range as CaptureDir takes them),
    oldest first, each numbered (`index`) for `dungeon-run --span`. Companions' visits join the span of the humans
    they overlap; a span is never made of companions alone. `maps` keeps only those map ids."""
    if isinstance(capture_dir, r.CaptureDir):
        cap = capture_dir
    else:
        cap = r.CaptureDir(capture_dir, date or start, date or end)
    by_instance: dict[tuple[int, int], list[Visit]] = {}
    for visit in visits(cap):
        if maps is not None and visit.map not in maps:
            continue
        by_instance.setdefault((visit.map, visit.instance), []).append(visit)
    spans: list[Span] = []
    for (map_id, instance), group in by_instance.items():
        humans = [v for v in group if v.kind == 0]
        humans.sort(key=lambda v: v.start_ms)
        current: Span | None = None
        for visit in humans:
            if current is not None:
                current_end = current.end_ms
                if current_end is None or visit.start_ms <= current_end + JOIN_MS:
                    current.visits.append(visit)
                    if visit.player not in current.players:
                        current.players.append(visit.player)
                    if current_end is not None:
                        current.end_ms = None if visit.end_ms is None else max(current_end, visit.end_ms)
                    continue
                spans.append(current)
            current = Span(map_id, instance, visit.start_ms, visit.end_ms, [visit.player], [], [visit])
        if current is not None:
            spans.append(current)
        for span in spans:
            if (span.map, span.instance) != (map_id, instance):
                continue
            lo, hi = span.window()
            for visit in group:
                if visit.kind == 0:
                    continue
                v_end = visit.end_ms if visit.end_ms is not None else hi
                if visit.start_ms <= hi and v_end >= lo:
                    span.visits.append(visit)
                    if visit.player not in span.companions:
                        span.companions.append(visit.player)
    spans.sort(key=lambda s: (s.start_ms, s.map, s.instance))
    for i, span in enumerate(spans):
        span.index = i
        span.visits.sort(key=lambda v: (v.start_ms, v.player))
    return spans


def find_span(spans: list[Span], index: int | None = None, map_id: int | None = None, instance: int | None = None,
              from_ms: int | None = None, to_ms: int | None = None) -> Span:
    """One span: by its `index` in the listing, or the (map, instance) one -- the longest when several, or the one
    overlapping [from_ms, to_ms] when given (and the window then cuts it)."""
    if index is not None:
        if index < 0 or index >= len(spans):
            raise IndexError(f"span {index}: the listing has {len(spans)}")
        return spans[index]
    if map_id is None:
        raise ValueError("a span is named by --span <index> or --map <id> [--instance <id>]")
    found = [s for s in spans if s.map == map_id and (instance is None or s.instance == instance)]
    if from_ms is not None or to_ms is not None:
        lo = from_ms if from_ms is not None else 0
        hi = to_ms if to_ms is not None else 1 << 62
        found = [s for s in found if s.start_ms <= hi and s.window()[1] >= lo]
    if not found:
        raise LookupError(f"no span of map {map_id}" + (f" instance {instance}" if instance is not None else ""))
    found.sort(key=lambda s: -(s.window()[1] - s.start_ms))
    span = found[0]
    if from_ms is not None or to_ms is not None:
        cut = Span(span.map, span.instance, max(span.start_ms, lo), min(span.window()[1], hi), list(span.players),
                   list(span.companions), list(span.visits), span.index)
        return cut
    return span


def count_records(cap: r.CaptureDir, span: Span) -> dict[str, int]:
    """Records per type in the map's files (and the session file) within the span's window: a measure of the
    visit's data."""
    lo, hi = span.window()
    counts: dict[str, int] = {}
    for hour in span.hours(cap):
        for path in hour.files():
            stream, shard = r.stream_map(path)
            if shard not in (str(span.map), "all"):
                continue
            for batch in r.read_file(path):
                for rtype, rows in batch.records.items():
                    if not len(rows) or "ms" not in rows.dtype.names:
                        continue
                    ms = rows["ms"]
                    inside = (ms >= lo) & (ms <= hi)
                    if "player" in rows.dtype.names and shard == "all":
                        members = np.isin(rows["player"], span.players + span.companions)
                        inside &= members
                    n = int(inside.sum())
                    if n:
                        name = r.PREFIX[rtype][0]
                        counts[name] = counts.get(name, 0) + n
    return counts
