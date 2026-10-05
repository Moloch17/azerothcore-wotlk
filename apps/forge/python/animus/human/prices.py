"""Proposed noise prices from human play (plan §2.6): a report only; it never writes config.

The forge charges steering that does not commit and presses that do nothing (CurriculumTuning.h ActionTuning,
MoveControls.h's Press): **Actions.Jitter** per quarter turn a press takes back of the choice before it, weighed by
recency e^(-dt / **Options.JitterDecayMs**) -- a reversal of the feet (forward to back, left to right) or of a climb
half a turn, of a turn or pitch rate the smaller rate over a quarter turn a second; **Actions.Effort** per press
(a rate by its size, EffortOf over 180 deg/s turning, 90 deg/s pitching); **Actions.Repeat** per press of
one action past RepeatFree (3) within RepeatWindowMs (10 s); **Actions.Fidget** per second moving in a fight while
already at the wanted range, once held **Actions.SettleGraceMs**.

Human presses come from the inverse mapper (movement, controller space) and the CastRequests (casts); the same
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


TURN_FULL = math.pi              # Press's full turn press: TURN_RATE_MAX / 2, 180 deg/s
PITCH_FULL = math.pi / 2        # ... and pitch press: PITCH_RATE_MAX, 90 deg/s


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


@dataclass
class _Axis:
    """One held control as MoveControls' SeatControls remembers it: the value held, the last non-zero one, and when
    the held value was last let go."""

    held: float = 0.0
    last: float = 0.0
    let_go: float = -1e9


def _press(tally: UnitTally, axis: _Axis, value: float, now: float, rate: float | None) -> None:
    """MoveControls::Press on one axis: a change to `value` at `now`; `rate` is the full press of a rate axis (None
    for a key). A reversal's since is 0 while the reversed value is still held (Detail::Change). The jitter is
    tallied as (amount, since) so jitter_per_min can price it at any decay."""
    if value == axis.held:
        return
    opposite = value != 0.0 and axis.last != 0.0 and (value > 0) != (axis.last > 0)
    since = 0.0 if axis.held != 0.0 else now - axis.let_go
    if rate is None:
        tally.effort += 1.0
        amount = 2.0
    else:
        tally.effort += min(1.0, abs(value if value != 0.0 else axis.held) / rate)
        amount = min(abs(value), abs(axis.last)) / QUARTER_TURN
    if opposite:
        tally.jitter_amount.append(amount)
        tally.jitter_since.append(max(0.0, since))
    if axis.held != 0.0:
        axis.let_go = now
    if value != 0.0:
        axis.last = value
    axis.held = value


def tally_movement(tally: UnitTally, samples: np.ndarray, actions: list[int], space: fit.Space | None = None) -> None:
    """Add one clip's mapped movement presses (`actions` per decision of `samples`) to a unit, priced as
    MoveControls::Press prices them."""
    space = space or fit.SPACES["controller"]
    dt_s = space.dt
    tally.minutes += (len(samples) - 1) * dt_s / 60.0
    axes = {kind: _Axis() for kind in (fit.FORWARD, fit.STRAFE, fit.VERTICAL, fit.TURN, fit.PITCH)}
    full = {fit.TURN: TURN_FULL, fit.PITCH: PITCH_FULL}
    for k, a in enumerate(actions):
        kind, value = space.actions[a]
        if kind == fit.NOOP:
            continue
        if kind in axes:
            _press(tally, axes[kind], value, k * dt_s, full.get(kind))
        else:
            tally.effort += 1.0             # JUMP, WALK_TOGGLE: a full press
    feats = motion.features(samples)
    moving = feats[:, motion.INDEX["moving"]] > 0.5
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
