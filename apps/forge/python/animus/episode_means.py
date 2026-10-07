"""Means of episode info columns, where some columns are per event rather than per episode.

An episode that never reached its marker reports arrive_seconds 0, not "nothing": averaged over every episode, M1's
first run read 5.8 s for a run whose optimum is 16.6 s (2026-10-05). A per-event column is the mean over the events
its episode had, so the right mean across episodes weights each episode by that count, and an episode with none of
them carries no weight at all. Everything else is a plain mean.
"""

from __future__ import annotations

import numpy as np

# Per-event column -> the column counting its events in the same episode.
PER_EVENT = {
    "arrive_seconds": "markers",
    "time_ratio": "markers",
    "overshoot": "markers",
    "stop_distance": "stops_near",
    # The seek stage (SeekEncounter): the arrival's clock and the rooms walked before it over the episodes that found
    # the object, the first sighting's clock over those that saw it, from sighting to arrival over those that did both,
    # and the revisit rate over every room entry.
    "find_seconds": "found",
    "rooms_before_found": "found",
    "sight_seconds": "sighted",
    "sight_to_arrival": "found_sighted",
    "revisit_rate": "room_entries",
    # ... and its found rate by the placement's rung (REDESIGN §2: the status headline per rung), over the episodes
    # placed at each.
    "found_hallway": "rung_hallway",
    "found_doorway": "rung_doorway",
    "found_room": "rung_room",
    "found_deep": "rung_deep",
    # M1 redesigned (SightEncounter): the arrival rate in the episodes that withheld the compass, and in those that
    # showed it.
    "arrived_no_compass": "compass_withheld",
    "arrived_with_compass": "compass_present",
    # ... and its time and time ratio by where the object stood: in sight of the spawn, or round a corner.
    "arrive_seconds_sight": "markers_sight",
    "time_ratio_sight": "markers_sight",
    "arrive_seconds_corner": "markers_corner",
    "time_ratio_corner": "markers_corner",
}


def means(values: np.ndarray, names: list[str] | tuple[str, ...]) -> np.ndarray:
    """Column means of `values` (episodes x columns, in `names` order); a per-event column whose count column is
    present is weighted by it, and is NaN when no episode had the event."""
    values = np.asarray(values, dtype=np.float64)
    if values.ndim != 2 or len(values) == 0:
        return np.full(len(names), np.nan)
    out = values.mean(axis=0)
    index = {name: i for i, name in enumerate(names)}
    for name, count in PER_EVENT.items():
        if name in index and count in index:
            weights = values[:, index[count]]
            total = weights.sum()
            out[index[name]] = float((values[:, index[name]] * weights).sum() / total) if total > 0 else np.nan
    return out
