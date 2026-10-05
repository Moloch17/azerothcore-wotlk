"""Proposed noise prices from human play (plan §2.6): a report only; it never writes config.

The forge charges steering that does not commit and presses that do nothing (CurriculumTuning.h ActionTuning,
MovePrice.h): **Actions.Jitter** per quarter turn a turn, pitch or bearing takes back of the one before, weighed by
recency e^(-dt / **Options.JitterDecayMs**), plus a facing mode taken back and a start after a stop at their
recency; **Actions.Effort** per press (a steering press by its angle, EffortOf); **Actions.Repeat** per press of
one action past RepeatFree (3) within RepeatWindowMs (10 s); **Actions.Fidget** per second moving in a fight while
already at the wanted range, once held **Actions.SettleGraceMs**.

Human presses come from the inverse mapper (movement, lattice space) and the CastRequests (casts); the same
quantities are tallied per unit (one player in one hour shard, at least MIN_MINUTES of clip time) and the prices
are proposed so that the **median human pays at most `budget`** per minute for each term:

- Jitter: budget / p50(jitter weight per minute) at the current decay; JitterDecayMs: the longest decay on
  DECAY_GRID at which the median human's jitter, at the current Jitter price, stays within budget.
- Effort, Repeat, Fidget: budget / p50(quantity per minute).
- RepeatFree (reported beside Repeat): p50 of the most presses of one spell a unit made within any 10 s.
- SettleGraceMs: p50 of the human's in-combat moving runs, rounded up to 250 ms -- a median run settles within it.

Fidget is a **proxy**: the forge charges moving while already at the range the goal wants with nothing to step out
of; the capture has no goal, so every second moving in combat counts. It overstates the human's fidget, and so
understates the price that would leave the median human at budget.
"""

from __future__ import annotations

import datetime as dt
import json
import math
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from animus.human import fit, motion

CURRENT = {"Actions.Jitter": 0.05, "Options.JitterDecayMs": 2500, "Actions.Effort": 0.004, "Actions.Repeat": 0.03,
           "Actions.RepeatFree": 3, "Actions.RepeatWindowMs": 10000, "Actions.Fidget": 0.01,
           "Actions.SettleGraceMs": 500}
DECAY_GRID = tuple(range(250, 5001, 250))
BUDGET = 0.005
MIN_MINUTES = 0.5
QUARTER_TURN = math.pi / 2


def undone(previous: float, now: float) -> float:
    """MovePrice::Undone: the quarter turns `now` takes back of `previous`."""
    if previous == 0.0 or now == 0.0 or (previous > 0) == (now > 0) or abs(now) >= math.pi - 0.01:
        return 0.0
    return min(abs(previous), abs(now)) / QUARTER_TURN


def bearing_swing(apart: int, count: int = fit.BEARINGS) -> float:
    """MovePrice::BearingSwing."""
    apart %= count
    return min(apart, count - apart) * (2 * math.pi / count) / QUARTER_TURN


@dataclass
class UnitTally:
    """One unit's noise: jitter events as (undone quarter turns, seconds since the choice undone), effort, repeats,
    and the moving runs in combat (the fidget proxy), over `minutes` of clip time."""

    minutes: float = 0.0
    jitter_amount: list[float] = field(default_factory=list)
    jitter_since: list[float] = field(default_factory=list)
    effort: float = 0.0
    repeated: int = 0
    max_in_window: int = 0
    combat_runs: list[float] = field(default_factory=list)

    def jitter_per_min(self, decay_ms: float) -> float:
        if self.minutes <= 0:
            return 0.0
        a = np.asarray(self.jitter_amount)
        s = np.asarray(self.jitter_since)
        return float((a * np.exp(-s * 1000.0 / decay_ms)).sum()) / self.minutes if len(a) else 0.0


def tally_movement(tally: UnitTally, samples: np.ndarray, actions: list[int], space: fit.Space | None = None) -> None:
    """Add one clip's mapped movement presses (`actions` per decision of `samples`) to a unit."""
    space = space or fit.SPACES["lattice"]
    dt_s = space.dt
    tally.minutes += (len(samples) - 1) * dt_s / 60.0
    last_turn = 0.0
    turn_t = -1e9
    turn_until = -1e9
    last_bearing = -1
    bearing_t = -1e9
    pitch_delta = 0.0
    pitch_t = -1e9
    pitch_until = -1e9
    mode, last_mode, mode_t = fit.FACE_MODE_HOLD, None, -1e9
    for k, a in enumerate(actions):
        now = k * dt_s
        kind, value = space.actions[a]
        if kind == fit.NOOP:
            continue
        if kind == fit.TURN:
            since = 0.0 if now < turn_until else now - turn_t
            u = undone(last_turn, value)
            if u > 0:
                tally.jitter_amount.append(u)
                tally.jitter_since.append(since)
            last_turn, turn_t = value, now
            turn_until = now + math.ceil(abs(value) / space.turn_step - 1e-3) * dt_s
            tally.effort += min(1.0, abs(value) / fit.TURN_RATE)
        elif kind == fit.BEARING:
            b = int(value)
            if last_bearing >= 0 and b != last_bearing:
                tally.jitter_amount.append(bearing_swing(b - last_bearing))
                tally.jitter_since.append(now - bearing_t)
            last_bearing, bearing_t = b, now
            tally.effort += 1.0
        elif kind == fit.PITCH:
            delta = value - float(samples[k, motion.PITCH])
            since = 0.0 if now < pitch_until else now - pitch_t
            u = undone(pitch_delta, delta)
            if u > 0:
                tally.jitter_amount.append(u)
                tally.jitter_since.append(since)
            pitch_delta, pitch_t = delta, now
            pitch_until = now + max(1, math.ceil(abs(delta) / space.pitch_step - 1e-3)) * dt_s
            tally.effort += min(1.0, abs(delta) / fit.TURN_RATE)
        elif kind in (fit.FACE_HEADING, fit.FACE_HOLD):
            new = fit.FACE_MODE_HEADING if kind == fit.FACE_HEADING else fit.FACE_MODE_HOLD
            if new != mode:
                if last_mode == new:
                    tally.jitter_amount.append(1.0)
                    tally.jitter_since.append(now - mode_t)
                last_mode, mode, mode_t = mode, new, now
            tally.effort += 1.0
        else:
            tally.effort += 1.0
    # Starts after a stop, at their recency (StageScenario: every start after a stop).
    feats = motion.features(samples)
    moving = feats[:, motion.INDEX["moving"]] > 0.5
    stopped_at = None
    for k in range(1, len(moving)):
        if moving[k] and not moving[k - 1] and stopped_at is not None:
            tally.jitter_amount.append(1.0)
            tally.jitter_since.append((k - stopped_at) * dt_s)
        if not moving[k] and moving[k - 1]:
            stopped_at = k
    # Fidget proxy: moving in combat, charged once a run has held the grace.
    combat = samples[1:, motion.IN_COMBAT] > 0.5
    run = 0
    for k in range(len(moving)):
        if moving[k] and combat[k]:
            run += 1
        elif run:
            tally.combat_runs.append(run * dt_s)
            run = 0
    if run:
        tally.combat_runs.append(run * dt_s)


def tally_casts(tally: UnitTally, cast_ms: np.ndarray, spells: np.ndarray, free: int = 3,
                window_ms: int = 10000) -> None:
    """Add CastRequests (times and spells, any order) to a unit: one effort each, and the repeats past `free`."""
    order = np.argsort(cast_ms, kind="stable")
    t, s = np.asarray(cast_ms)[order].astype(np.int64), np.asarray(spells)[order]
    tally.effort += len(t)
    for spell in np.unique(s):
        times = t[s == spell]
        inside = np.searchsorted(times, times, side="right") - np.searchsorted(times, times - window_ms, side="left")
        tally.repeated += int((inside > free).sum())
        tally.max_in_window = max(tally.max_in_window, int(inside.max()))


def _p50(values: list[float]) -> float | None:
    return float(np.percentile(values, 50)) if values else None


def propose(units: list[UnitTally], budget: float = BUDGET, current: dict | None = None,
            grace_ms: float | None = None) -> dict:
    """The proposal from per-unit tallies (see the module docstring)."""
    current = dict(CURRENT, **(current or {}))
    units = [u for u in units if u.minutes >= MIN_MINUTES]
    grace = (grace_ms if grace_ms is not None else current["Actions.SettleGraceMs"]) / 1000.0
    out: dict = {"format": 1, "built": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
                 "units": len(units), "budget_per_minute": budget, "current": current, "proposed": {},
                 "median_human": {}, "notes": ["a report only: nothing here is written to config",
                                               "fidget is a proxy: every second moving in combat past the grace"]}
    if not units:
        return out
    jitter = [u.jitter_per_min(current["Options.JitterDecayMs"]) for u in units]
    effort = [u.effort / u.minutes for u in units]
    repeat = [u.repeated / u.minutes for u in units]
    fidget = [sum(max(0.0, r - grace) for r in u.combat_runs) / u.minutes for u in units]
    runs = [r for u in units for r in u.combat_runs]
    med = {"jitter_weight_per_min": _p50(jitter), "effort_per_min": _p50(effort),
           "repeated_presses_per_min": _p50(repeat), "fidget_seconds_per_min": _p50(fidget),
           "max_same_spell_in_window": _p50([u.max_in_window for u in units]),
           "combat_moving_run_seconds": _p50(runs)}
    out["median_human"] = {k: (round(v, 5) if v is not None else None) for k, v in med.items()}
    out["median_human_cost_per_min_now"] = {
        "jitter": round(current["Actions.Jitter"] * med["jitter_weight_per_min"], 6),
        "effort": round(current["Actions.Effort"] * med["effort_per_min"], 6),
        "repeat": round(current["Actions.Repeat"] * med["repeated_presses_per_min"], 6),
        "fidget": round(current["Actions.Fidget"] * med["fidget_seconds_per_min"], 6)}

    def cap(quantity: float | None, now: float) -> float:
        return round(now if not quantity else min(now, budget / quantity), 6)

    prop = out["proposed"]
    prop["Actions.Jitter"] = cap(med["jitter_weight_per_min"], current["Actions.Jitter"])
    ok = [tau for tau in DECAY_GRID
          if current["Actions.Jitter"] * _p50([u.jitter_per_min(tau) for u in units]) <= budget]
    prop["Options.JitterDecayMs"] = max(ok) if ok else min(DECAY_GRID)
    prop["Actions.Effort"] = cap(med["effort_per_min"], current["Actions.Effort"])
    prop["Actions.Repeat"] = cap(med["repeated_presses_per_min"], current["Actions.Repeat"])
    prop["Actions.RepeatFree"] = max(current["Actions.RepeatFree"], int(math.ceil(med["max_same_spell_in_window"])))
    prop["Actions.Fidget"] = cap(med["fidget_seconds_per_min"], current["Actions.Fidget"])
    if med["combat_moving_run_seconds"] is not None:
        prop["Actions.SettleGraceMs"] = int(math.ceil(med["combat_moving_run_seconds"] * 1000 / 250.0) * 250)
    return out


def markdown(report: dict) -> str:
    lines = ["# Noise prices proposed from human play", "",
             f"{report['units']} units; budget {report['budget_per_minute']} per minute per term for the median "
             "human. A report only: nothing is written to config.", "",
             "| key | current | proposed |", "|---|---|---|"]
    for key, value in report.get("proposed", {}).items():
        lines.append(f"| {key} | {report['current'].get(key)} | {value} |")
    lines += ["", "Median human: " + ", ".join(f"{k} {v}" for k, v in report.get("median_human", {}).items()), "",
              "Notes: " + "; ".join(report["notes"]) + "."]
    return "\n".join(lines) + "\n"


def write(report: dict, out_dir: str | Path) -> tuple[Path, Path]:
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    js, md = out_dir / "human_prices.json", out_dir / "human_prices.md"
    js.write_text(json.dumps(report, indent=1))
    md.write_text(markdown(report))
    return js, md
