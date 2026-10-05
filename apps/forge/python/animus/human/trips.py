"""human_trips.json (FORMAT.md §5): where humans actually go, per map, for the forge's TravelEncounter trip pools.

```
{"format": 1, "maps": {"<map>": [{"start": [x, y, z], "end": [x, y, z], "seconds": s,
                                  "mode": "ground|swim|fly|mounted", "path": [[x, y, z], ...]}]}}
```

Exactly that and nothing more: the forge's reader refuses the whole file over one malformed entry, so every entry
is checked (`valid_trip`) before it is written and anything else -- when and from what it was built, how many trips
each map saw, how each trip ended (a stay or an interaction) -- goes to the sidecar `human_trips.meta.json`.

Trips come from segment.trips (hindsight destinations: the next place the player stopped and stayed, or
interacted). `path` is a point every 2 s of the trip plus its end. Each map keeps at most `per_map` trips, a
uniform sample of all seen on it (bottom-k on random keys).
"""

from __future__ import annotations

import datetime as dt
import json
import math
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from animus.human.segment import Trip

FORMAT = 1
FILE = "human_trips.json"
META_FILE = "human_trips.meta.json"
MODES = ("ground", "swim", "fly", "mounted")


def _point(value) -> bool:
    return (isinstance(value, list) and len(value) == 3
            and all(isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) for v in value))


def valid_trip(item: dict) -> bool:
    """Whether a trip entry is exactly what the forge reader accepts."""
    if set(item) - {"start", "end", "seconds", "mode", "path"} or not {"start", "end", "seconds", "mode"} <= set(item):
        return False
    seconds = item["seconds"]
    if not _point(item["start"]) or not _point(item["end"]) or item["mode"] not in MODES:
        return False
    if not isinstance(seconds, (int, float)) or isinstance(seconds, bool) or not math.isfinite(seconds) \
            or seconds < 0:
        return False
    return "path" not in item or (isinstance(item["path"], list) and all(_point(p) for p in item["path"]))


@dataclass
class TripPool:
    per_map: int = 5000
    seed: int = 0
    maps: dict[int, list[tuple[float, dict]]] = field(default_factory=dict)
    seen: dict[int, int] = field(default_factory=dict)
    destinations: dict[str, int] = field(default_factory=dict)
    rejected: int = 0

    def __post_init__(self):
        self._rng = np.random.default_rng(self.seed)

    def add(self, trip: Trip) -> None:
        item = {"start": [float(v) for v in trip.start], "end": [float(v) for v in trip.end],
                "seconds": float(trip.seconds), "mode": trip.mode, "path": [[float(v) for v in p] for p in trip.path]}
        if not valid_trip(item):
            self.rejected += 1
            return
        pool = self.maps.setdefault(int(trip.map), [])
        self.seen[int(trip.map)] = self.seen.get(int(trip.map), 0) + 1
        self.destinations[trip.destination] = self.destinations.get(trip.destination, 0) + 1
        pool.append((float(self._rng.random()), item))
        if len(pool) > 2 * self.per_map:
            pool.sort(key=lambda kv: kv[0])
            del pool[self.per_map:]

    def result(self) -> dict:
        maps = {}
        for mapid in sorted(self.maps):
            pool = sorted(self.maps[mapid], key=lambda kv: kv[0])[:self.per_map]
            maps[str(mapid)] = [item for _, item in pool]
        return {"format": FORMAT, "maps": maps}

    def meta(self, source: dict) -> dict:
        return {"format": FORMAT, "built": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
                "source": source, "per_map": self.per_map,
                "trips_seen": {str(k): v for k, v in sorted(self.seen.items())},
                "destinations": self.destinations, "rejected": self.rejected}

    def write(self, out_dir: str | Path, source: dict) -> Path:
        out_dir = Path(out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        result = self.result()
        assert all(valid_trip(t) for trips in result["maps"].values() for t in trips)
        path = out_dir / FILE
        path.write_text(json.dumps(result))
        (out_dir / META_FILE).write_text(json.dumps(self.meta(source)))
        return path
