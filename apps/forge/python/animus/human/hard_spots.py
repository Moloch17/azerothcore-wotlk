"""human_hard_spots.json (FORMAT.md §5): where humans die, get stuck, fall and drown, for start pools.

```
{"format": 1, "maps": {"<map>": [{"pos": [x, y, z], "kind": "death|stuck|fall|drown", "count": n}]}}
```

Exactly that: the forge's reader refuses the whole file over one malformed entry, so every spot is checked
(`valid_spot`) before writing, and the rest -- build time, source, cluster size, and the death causes behind each
`death` spot (by map and spot index) -- goes to the sidecar `human_hard_spots.meta.json`.

Kinds: `death` -- a Death by a creature, a player, fire/lava or other (the sidecar counts which); `drown` -- a Death
by drowning; `fall` -- a Death by falling, or a landing (MSG_MOVE_FALL_LAND) after LONG_FALL_MS or more in the air,
survived or not; `stuck` -- a movement key held STUCK_SECONDS with the body moving under STUCK_YARDS
(segment.stuck_spans), at where it stood. Points are clustered per map and kind on a `cluster_yards` grid (a cell's
points become one spot at their mean, with their count); spots are listed by count, most first.
"""

from __future__ import annotations

import datetime as dt
import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from animus.human.tracks import MSG_MOVE_FALL_LAND

FORMAT = 1
FILE = "human_hard_spots.json"
META_FILE = "human_hard_spots.meta.json"
KINDS = ("death", "stuck", "fall", "drown")
LONG_FALL_MS = 1500
CLUSTER_YARDS = 8.0
KIND_OF_CAUSE = {"fall": "fall", "drowning": "drown"}


def valid_spot(spot: dict) -> bool:
    """Whether a spot entry is exactly what the forge reader accepts."""
    pos, count = spot.get("pos"), spot.get("count")
    return (set(spot) == {"pos", "kind", "count"} and spot["kind"] in KINDS
            and isinstance(pos, list) and len(pos) == 3
            and all(isinstance(v, float) and np.isfinite(v) for v in pos)
            and isinstance(count, int) and not isinstance(count, bool) and count >= 1)


@dataclass
class HardSpots:
    cluster_yards: float = CLUSTER_YARDS
    points: dict[tuple[int, str], list[tuple[float, float, float, str]]] = field(default_factory=dict)

    def add(self, mapid: int, kind: str, pos, cause: str = "") -> None:
        if kind not in KINDS or not np.all(np.isfinite(np.asarray(pos, dtype=np.float64))):
            return
        self.points.setdefault((int(mapid), kind), []).append((float(pos[0]), float(pos[1]), float(pos[2]), cause))

    def add_deaths(self, deaths: list[dict]) -> None:
        for death in deaths:
            self.add(death["map"], KIND_OF_CAUSE.get(death["cause"], "death"), death["pos"], death["cause"])

    def add_falls(self, moves: np.ndarray, companions: set | None = None) -> None:
        landed = moves[(moves["opcode"] == MSG_MOVE_FALL_LAND) & (moves["fall_ms"] >= LONG_FALL_MS)
                       & (moves["source"] == 0)]
        for row in landed:
            if companions and int(row["player"]) in companions:
                continue
            self.add(int(row["map"]), "fall", (row["x"], row["y"], row["z"]), "landed")

    def result(self) -> tuple[dict, dict]:
        """The spots file and the death causes behind its `death` spots ({map: {spot index: {cause: n}}})."""
        maps: dict[str, list] = {}
        causes_of: dict[str, list] = {}
        for (mapid, kind), pts in sorted(self.points.items()):
            xyz = np.asarray([p[:3] for p in pts], dtype=np.float64)
            causes = [p[3] for p in pts]
            cells = np.floor(xyz / self.cluster_yards).astype(np.int64)
            _, inverse = np.unique(cells, axis=0, return_inverse=True)
            inverse = inverse.reshape(-1)
            for cell in range(inverse.max() + 1):
                rows = np.flatnonzero(inverse == cell)
                spot = {"pos": [round(float(v), 2) for v in xyz[rows].mean(axis=0)], "kind": kind,
                        "count": int(len(rows))}
                mine = [causes[i] for i in rows]
                maps.setdefault(str(mapid), []).append(spot)
                causes_of.setdefault(str(mapid), []).append({c: mine.count(c) for c in sorted(set(mine)) if c})
        causes_out: dict[str, dict] = {}
        for key, spots in maps.items():
            order = sorted(range(len(spots)), key=lambda i: -spots[i]["count"])
            maps[key] = [spots[i] for i in order]
            causes_out[key] = {str(n): causes_of[key][i] for n, i in enumerate(order)
                               if spots[i]["kind"] == "death"}
        return {"format": FORMAT, "maps": maps}, causes_out

    def write(self, out_dir: str | Path, source: dict) -> Path:
        out_dir = Path(out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        result, causes = self.result()
        assert all(valid_spot(s) for spots in result["maps"].values() for s in spots)
        path = out_dir / FILE
        path.write_text(json.dumps(result))
        (out_dir / META_FILE).write_text(json.dumps(
            {"format": FORMAT, "built": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
             "source": source, "cluster_yards": self.cluster_yards, "death_causes": causes}))
        return path
