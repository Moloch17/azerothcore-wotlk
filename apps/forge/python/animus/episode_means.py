"""Means of episode info columns, where some columns are per event rather than per episode.

An episode that never reached its marker reports arrive_seconds 0, not "nothing": averaged over every episode, M1's
first run read 5.8 s for a run whose optimum is 16.6 s (2026-10-05). A per-event column is the mean over the events
its episode had, so the right mean across episodes weights each episode by that count, and an episode with none of
them carries no weight at all. Everything else is a plain mean.
"""

from __future__ import annotations

import numpy as np

class AnyOf(tuple):
    """Count columns in order of preference: the FIRST one the log has weights the per-event column (a tuple would sum
    them). For a reading the sim writes under one of two names (pause_look_share over `pauses`, else the per-minute
    rate `pauses_per_min` as the stand-in)."""


# Per-event column -> the column counting its events in the same episode, or several (their sum): a column whose events
# come from either of two sources (goal_follow_rate: room goals or cell goals, an episode has one of the two); AnyOf
# names alternatives instead, the first present. Columns the stage names (found_arena_<arena> over arena_<arena>) are
# matched by per_event_count's prefix rule, not listed here.
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
    # ... and the room goals (m2-goals): the share of chosen room goals that were reached over the goals chosen, and the
    # object room's coverage when the object was found, and whether that room had been Checked before it was, over the
    # episodes that found it.
    # ... and, from free-choice-goals, over either source's chosen goals (an episode has the one it played).
    "goal_follow_rate": ("goals_room_chosen", "goals_cell_chosen"),
    "check_cover_at_find": "found",
    "checked_miss": "found",
    # ... and the cell goals (free-choice-goals): how long a cell goal ran, how far off it was chosen, the share chosen on
    # ground already stood on, and the plan's depth at a new choice, over the cell goals chosen.
    "cell_goal_seconds": "goals_cell_chosen",
    "cell_goal_yards": "goals_cell_chosen",
    "cell_stale_share": "goals_cell_chosen",
    # ... and the trap drill (explore-unstuck): got out over the episodes that started in the trap pose, the clock over
    # those that got out.
    "trap_escaped": "trap_episode",
    "trap_escape_seconds": "trap_escaped",
    # ... and exploration v3: whether the seat turned >= Seek.TrapEscapeTurnDeg and the charged wall seconds inside the
    # escape window, over the trap episodes only (the other episodes log 0).
    "trap_turned": "trap_episode",
    "trap_pin_seconds": "trap_episode",
    "plan_depth": "goals_cell_chosen",
    # ... and movement pacing (general-search contract M7; the sim reads the controller's body): the seconds from a
    # pin's onset to 4 yd away and the share of pins the seat ran straight into, over the pin events (Stuck runs of a
    # second or more); the share of pauses with a look round, over the pauses (`pauses` when the sim writes the count,
    # else the per-minute rate stands in).
    "time_to_escape_s": "pin_events",
    "pin_onset_turning_share": "pin_events",
    "pause_look_share": AnyOf(("pauses", "pauses_per_min")),
    # ... and exploration v2 (decision 0025): the clock the Explore cap was reached at, over the episodes that reached it
    # (explore_cap_hit_ms is -1 for the others).
    "explore_cap_hit_ms": "explore_cap_hit",
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
    # M3 interact (InteractEncounter): the right object's clock over the episodes that got it, and the right object
    # by rung over the episodes that played each (sight_seconds and sight_to_arrival read as the seek stage's).
    "right_seconds": "right_object",
    "right_distinguish": "rung_distinguish",
    "right_switch": "rung_switch",
    "right_key": "rung_key",
    # M4 follow (PartyFollowEncounter): the regroups over the leader's stops each follower was counted for, their time
    # over the regroups; the rejoins after a rise at the entrance (I4) over the rises, their time over the rejoins.
    "regroup_share": "regroup_stops",
    "regroup_seconds": "regroups",
    "rejoined": "rises",
    "rejoin_seconds": "rejoins",
    # A whole dungeon's (InstanceEncounter, I4 wired into the wing runs): the time from a rise at the entrance to the
    # party, over the rejoins.
    "wing_rejoin_seconds": "wing_rejoins",
    # The combat stages (CombatEncounter): a C1 creature's engage-to-death over the kills.
    "kill_seconds": "kills",
    # ... and C2's watch: InterruptLanded's earnings over Kill and Clear's (a ratio of the sums).
    "interrupt_earnings": "outcome_paid",
    # G1 roles (RolesEncounter): each drill's win and reading over the episodes that drilled it, the share of packs
    # pulled alone over the packs cleared, and the party's rejoins as M4's and C3's.
    "won_hold": "drill_hold",
    "won_keep": "drill_keep",
    "won_focus": "drill_focus",
    "won_pull": "drill_pull",
    "hold_share": "drill_hold",
    "kept_share": "drill_keep",
    "low_mana_seconds": "drill_keep",
    "focus_share": "drill_focus",
    "pulled_seconds": "drill_focus",
    "clean_share": "packs_cleared",
    # The party stages (InstanceEncounter, dungeon-curriculum G2-D3): H's split -- the run's success over the episodes
    # with the "human" stand-in in a seat, and over the all-bot ones -- and each place's deaths over the rows of that
    # place (by role, read off the build).
    "clear_standin": "with_stand_in",
    "clear_allbot": "without_stand_in",
    "deaths_tank": "role_tank",
    "deaths_healer": "role_healer",
    "deaths_damage": "role_damage",
}


#: A per-event column named per stage: `found_arena_<arena>` (found, on that arena) is weighted by `arena_<arena>` (1
#: when the episode's arena is it), for every Seek arena the sim names (general search sec 1.5).
PER_EVENT_PREFIXES = {"found_arena_": "arena_"}


def per_event_count(name: str):
    """The count column(s) weighting per-event column `name`: PER_EVENT's entry, else the prefix rule's
    (found_arena_rooms -> arena_rooms), else None (a plain mean)."""
    count = PER_EVENT.get(name)
    if count is not None:
        return count
    for prefix, counted in PER_EVENT_PREFIXES.items():
        if name.startswith(prefix) and len(name) > len(prefix):
            return counted + name[len(prefix):]
    return None


def per_event_names(names) -> dict:
    """Per-event column -> its count column(s), for the columns in `names` (PER_EVENT and the prefix rule)."""
    out = {}
    for name in names:
        count = per_event_count(name)
        if count is not None:
            out[name] = count
    return out


def count_columns(count) -> tuple[str, ...]:
    """The columns PER_EVENT's value names (one, or several whose sum counts the events, or AnyOf's alternatives)."""
    return (count,) if isinstance(count, str) else tuple(count)


def event_weights(count, column) -> np.ndarray | None:
    """A per-event column's weights: the sum of the count columns that exist (the first that exists for AnyOf).
    `column(name)` gives a column's values per episode, or None when the log has none; None when none of them exists."""
    found = [values for values in (column(name) for name in count_columns(count)) if values is not None]
    if not found:
        return None
    if isinstance(count, AnyOf):
        return np.asarray(found[0], dtype=np.float64)
    return sum(np.asarray(values, dtype=np.float64) for values in found)


def means(values: np.ndarray, names: list[str] | tuple[str, ...]) -> np.ndarray:
    """Column means of `values` (episodes x columns, in `names` order); a per-event column whose count column is
    present is weighted by it, and is NaN when no episode had the event."""
    values = np.asarray(values, dtype=np.float64)
    if values.ndim != 2 or len(values) == 0:
        return np.full(len(names), np.nan)
    out = values.mean(axis=0)
    index = {name: i for i, name in enumerate(names)}
    for name, count in per_event_names(names).items():
        weights = event_weights(count, lambda c: values[:, index[c]] if c in index else None)
        if weights is not None:
            total = weights.sum()
            out[index[name]] = float((values[:, index[name]] * weights).sum() / total) if total > 0 else np.nan
    return out


def undefined(values: np.ndarray, names: list[str] | tuple[str, ...]) -> set[str]:
    """The per-event columns `means` leaves NaN because no episode had the event: nothing to average, not a fault. A
    rung-conditional column (found_room before the ladder reaches the placement) is one, until its rung is played."""
    values = np.asarray(values, dtype=np.float64)
    if values.ndim != 2 or len(values) == 0:
        return set()
    index = {name: i for i, name in enumerate(names)}
    out = set()
    for name, count in per_event_names(names).items():
        weights = event_weights(count, lambda c: values[:, index[c]] if c in index else None)
        if weights is not None and weights.sum() <= 0:
            out.add(name)
    return out
