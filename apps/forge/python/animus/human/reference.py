"""human_reference.json: what human play looks like, for the realism score and for `bot vs human` in eval reports.

```
{"format": 1, "built": "<iso>", "source": {"capture_dir", "from", "to", "players", "hours"}, "step_seconds": 0.25,
 "motion": {"<context id>": {"name": motion.context_name, "steps": N, "hist": {"<feature>": [counts]}}},
 "metrics": {"<class>/<spec tree>/<level band>": {"<metric>": {"p10", "p50", "p90", "n"}}}}
```

`hist` is counted in motion.HIST_BINS (fixed, not stored) over every usable step of every human clip.
`source.hours` is the number of hour shards read (hour directories in the range that held a move file), and
`source.players` the distinct non-companion players seen. Metric keys also have the roll-ups `<class>/all/all` and
`all/all/all`; a player whose session record was not seen is `unknown/unknown/unknown`.

**Metrics** are per *unit* -- one player in one hour shard -- and their percentiles are taken across units (`n`
units). A unit enters a movement metric only with at least MIN_MOVING_MINUTES of moving time in usable clips, an
action metric only with at least MIN_ACTIVE_MINUTES between its first and last record. Definitions (all on the
0.25 s grid, over usable clip steps: no involuntary, idle or cut motion):

- `turn_reversals_per_min`: a *turn* is a run of consecutive steps turning the same way at TURN_MIN_RATE or more,
  of at least TURN_MIN_ANGLE in all. A reversal is a turn the other way from the one before it starting within
  REVERSAL_WINDOW (1.5 s, MovePrice::COUNT_MS) of that turn's end; a turn of half a revolution or more is never one
  (MovePrice::Undone). Per minute of clip time. The forge's `turn_reversals` counts presses; this counts the
  human's continuous turns the same way.
- `stop_starts_per_min`: a step that starts moving (planar over motion.MOVING after a step under it) within
  STOP_START_WINDOW (1 s) of the last stop, per minute -- the forge's move_stop_starts.
- `strafe_share`: of moving steps, those whose course (the direction of travel relative to facing) is 22.5-112.5
  degrees either side: a sideways component, forward diagonals included.
- `backpedal_share`: of moving steps, those with a course over 112.5 degrees: back and back diagonals.
- `jumps_per_min`: MSG_MOVE_JUMP packets per minute of clip time (human only: the bot side has no packets; a bot's
  jumps are counted from the grid as ground -> airborne steps by `movement_counts` when no packets are given).
- `planar_speed_ratio`: mean planar speed of moving steps, in units of the speed in force (motion's `planar`).
- `swim_share`, `fly_share`: shares of all steps swimming / flying.
- `casts_per_min`: CastRequests per active minute (first to last record of the player in the shard).
- `cast_fail_rate`: of CastResults, the share that failed (result != 255); `cast_fail_<code>`: the share with one
  SpellCastResult code (the codes are the core's SpellCastResult enum), for every code seen.
- `overheal_share`: overheal / (amount) of the player's Heal records (amount includes the overheal, as the core
  reports it).
- `dps`: damage dealt in fights / fight seconds (fights of at least MIN_FIGHT_SECONDS; segment.fights).
"""

from __future__ import annotations

import datetime as dt
import json
import math
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from animus.human import motion
from animus.human import reader as r

FORMAT = 1
TURN_MIN_RATE = math.radians(10.0)      # rad/s: slower is drift, not a turn
TURN_MIN_ANGLE = math.radians(5.0)
REVERSAL_WINDOW = 1.5
STOP_START_WINDOW = 1.0
MIN_MOVING_MINUTES = 0.5
MIN_ACTIVE_MINUTES = 1.0
MIN_FIGHT_SECONDS = 5.0
STRAFE_FROM, STRAFE_TO = math.radians(22.5), math.radians(112.5)

CLASS_NAMES = {1: "warrior", 2: "paladin", 3: "hunter", 4: "rogue", 5: "priest", 6: "deathknight", 7: "shaman",
               8: "mage", 9: "warlock", 11: "druid"}
# Talent trees in tab order (tree_points[0..2]), as the forge's layout manifests name the specs.
TREE_NAMES = {
    "warrior": ("arms", "fury", "protection"), "paladin": ("holy", "protection", "retribution"),
    "hunter": ("beast_mastery", "marksmanship", "survival"), "rogue": ("assassination", "combat", "subtlety"),
    "priest": ("discipline", "holy", "shadow"), "deathknight": ("blood", "frost", "unholy"),
    "shaman": ("elemental", "enhancement", "restoration"), "mage": ("arcane", "fire", "frost"),
    "warlock": ("affliction", "demonology", "destruction"), "druid": ("balance", "feral", "restoration"),
}


def level_band(level: int) -> str:
    if level >= 80:
        return "80"
    if level < 10:
        return "1-9"
    lo = (level // 10) * 10
    return f"{lo}-{lo + 9}"


def group_key(info: dict) -> str:
    """`<class>/<spec tree>/<level band>` of a session (SessionTable.info); untalented below level 10."""
    if not info:
        return "unknown/unknown/unknown"
    cls = CLASS_NAMES.get(int(info.get("class_", 0)), f"class{int(info.get('class_', 0))}")
    points = list(info.get("tree_points") or [0, 0, 0])
    trees = TREE_NAMES.get(cls, ("tree0", "tree1", "tree2"))
    spec = trees[int(np.argmax(points))] if max(points) > 0 else "untalented"
    return f"{cls}/{spec}/{level_band(int(info.get('level', 0)))}"


def turns(yaw_step: np.ndarray, dt: float = motion.DECISION_SECONDS) -> list[tuple[int, int, float]]:
    """(start, end exclusive, angle) of each turn in a run of per-step yaw changes (radians, left positive)."""
    yaw_step = np.asarray(yaw_step, dtype=np.float64)
    sign = np.where(np.abs(yaw_step) >= TURN_MIN_RATE * dt, np.sign(yaw_step), 0).astype(int)
    out = []
    i = 0
    n = len(sign)
    while i < n:
        if sign[i] == 0:
            i += 1
            continue
        j = i
        while j < n and sign[j] == sign[i]:
            j += 1
        angle = float(yaw_step[i:j].sum())
        if abs(angle) >= TURN_MIN_ANGLE:
            out.append((i, j, angle))
        i = j
    return out


def turn_reversals(yaw_step: np.ndarray, dt: float = motion.DECISION_SECONDS) -> int:
    count = 0
    found = turns(yaw_step, dt)
    for (_, prev_end, prev_angle), (start, _, angle) in zip(found, found[1:]):
        if (prev_angle > 0) != (angle > 0) and abs(angle) < math.pi - 0.01 \
                and (start - prev_end) * dt < REVERSAL_WINDOW:
            count += 1
    return count


def movement_counts(feats: np.ndarray, jumps: np.ndarray | None = None, dt: float = motion.DECISION_SECONDS
                    ) -> dict[str, float]:
    """Additive counts over one unbroken run of steps (motion.features rows): sums that `movement_metrics` turns
    into rates. `jumps` (per step) are packet counts; without them a jump is a ground -> airborne step."""
    f = np.asarray(feats)
    n = len(f)
    if n == 0:
        return {}
    moving = f[:, motion.INDEX["moving"]] > 0.5
    course = np.abs(np.arctan2(f[:, motion.INDEX["course_sin"]], f[:, motion.INDEX["course_cos"]]))
    yaw_step = f[:, motion.INDEX["yaw_rate"]] * math.pi * dt
    starts = np.flatnonzero(moving[1:] & ~moving[:-1]) + 1
    stops = np.flatnonzero(~moving[1:] & moving[:-1]) + 1
    stop_starts = 0
    for s in starts:
        before = stops[stops < s]
        if len(before) and (s - before[-1]) * dt <= STOP_START_WINDOW:
            stop_starts += 1
    mode = np.argmax(f[:, motion.INDEX["mode_ground"]:motion.INDEX["mode_airborne"] + 1], axis=1)
    if jumps is None:
        jump_count = int(((mode[1:] == motion.MODE_AIRBORNE) & (mode[:-1] == motion.MODE_GROUND)).sum())
    else:
        jump_count = int(np.asarray(jumps).sum())
    return {"steps": n, "minutes": n * dt / 60.0, "moving": int(moving.sum()),
            "moving_minutes": float(moving.sum()) * dt / 60.0,
            "strafe": int((moving & (course >= STRAFE_FROM) & (course <= STRAFE_TO)).sum()),
            "backpedal": int((moving & (course > STRAFE_TO)).sum()),
            "reversals": turn_reversals(yaw_step, dt), "stop_starts": stop_starts, "jumps": jump_count,
            "planar_sum": float(f[moving, motion.INDEX["planar"]].sum()),
            "swim": int((mode == motion.MODE_SWIM).sum()), "fly": int((mode == motion.MODE_FLY).sum())}


def add_counts(total: dict, more: dict) -> dict:
    for key, value in more.items():
        total[key] = total.get(key, 0) + value
    return total


def movement_metrics(counts: dict) -> dict[str, float]:
    """Rates from summed `movement_counts`; empty when there is too little moving time to say."""
    if counts.get("moving_minutes", 0.0) < MIN_MOVING_MINUTES:
        return {}
    minutes, moving = counts["minutes"], max(1, counts["moving"])
    return {"turn_reversals_per_min": counts["reversals"] / minutes,
            "stop_starts_per_min": counts["stop_starts"] / minutes,
            "strafe_share": counts["strafe"] / moving, "backpedal_share": counts["backpedal"] / moving,
            "jumps_per_min": counts["jumps"] / minutes, "planar_speed_ratio": counts["planar_sum"] / moving,
            "swim_share": counts["swim"] / counts["steps"], "fly_share": counts["fly"] / counts["steps"]}


MOVEMENT_METRICS = ("turn_reversals_per_min", "stop_starts_per_min", "strafe_share", "backpedal_share",
                    "jumps_per_min", "planar_speed_ratio", "swim_share", "fly_share")


def action_metrics(actions: r.Batch, outcomes: r.Batch, player: int, fights: list[dict]) -> dict[str, float]:
    """Cast, failure, overheal and dps metrics of one player in one shard (see the module docstring)."""
    req = actions.get(r.CAST_REQUEST)
    res = actions.get(r.CAST_RESULT)
    req, res = req[req["player"] == player], res[res["player"] == player]
    heals = outcomes.get(r.HEAL)
    heals = heals[heals["source"] == player]
    times = np.concatenate([req["ms"], res["ms"]]).astype(np.float64)
    out: dict[str, float] = {}
    active = (times.max() - times.min()) / 60000.0 if len(times) else 0.0
    if active >= MIN_ACTIVE_MINUTES:
        out["casts_per_min"] = len(req) / active
    if len(res):
        failed = res["result"] != 255
        out["cast_fail_rate"] = float(failed.mean())
        for code in np.unique(res["result"][failed]):
            out[f"cast_fail_{int(code)}"] = float((res["result"] == code).mean())
    amount = float(heals["amount"].astype(np.float64).sum())
    if amount > 0:
        out["overheal_share"] = float(heals["overheal"].astype(np.float64).sum()) / amount
    long = [f for f in fights if f["seconds"] >= MIN_FIGHT_SECONDS]
    seconds = sum(f["seconds"] for f in long)
    if seconds > 0:
        out["dps"] = sum(f["dealt"] for f in long) / seconds
    return out


def percentiles(values: list[float]) -> dict:
    v = np.asarray(values, dtype=np.float64)
    p10, p50, p90 = np.percentile(v, [10, 50, 90])
    return {"p10": round(float(p10), 5), "p50": round(float(p50), 5), "p90": round(float(p90), 5), "n": len(v)}


@dataclass
class ReferenceBuilder:
    """Accumulates motion histograms per context and metric values per unit, shard by shard."""

    hist: dict[int, dict[str, np.ndarray]] = field(default_factory=dict)
    steps: dict[int, int] = field(default_factory=dict)
    units: dict[str, dict[str, list[float]]] = field(default_factory=dict)
    players: set = field(default_factory=set)
    hours: set = field(default_factory=set)

    def add_steps(self, feats: np.ndarray, contexts: np.ndarray) -> None:
        for context, per in motion.histograms(feats, contexts).items():
            mine = self.hist.setdefault(context, {name: np.zeros(len(motion.HIST_BINS[name]) - 1, dtype=np.int64)
                                                  for name in motion.HIST_FEATURES})
            for name, counts in per.items():
                mine[name] += counts
            self.steps[context] = self.steps.get(context, 0) + int((np.asarray(contexts) == context).sum())

    def add_unit(self, info: dict, metrics: dict[str, float]) -> None:
        if not metrics:
            return
        key = group_key(info)
        cls = key.split("/", 1)[0]
        for group in {key, f"{cls}/all/all", "all/all/all"}:
            bucket = self.units.setdefault(group, {})
            for name, value in metrics.items():
                if np.isfinite(value):
                    bucket.setdefault(name, []).append(float(value))

    def result(self, source: dict) -> dict:
        motion_out = {}
        for context in sorted(self.hist):
            motion_out[str(context)] = {"name": motion.context_name(context), "steps": self.steps.get(context, 0),
                                        "hist": {name: [int(c) for c in counts]
                                                 for name, counts in self.hist[context].items()}}
        metrics = {}
        for group in sorted(self.units):
            values = self.units[group]
            ordered = [m for m in MOVEMENT_METRICS if m in values] + sorted(m for m in values
                                                                           if m not in MOVEMENT_METRICS)
            metrics[group] = {name: percentiles(values[name]) for name in ordered}
        return {"format": FORMAT, "built": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
                "source": dict(source, players=len(self.players), hours=len(self.hours)),
                "step_seconds": motion.DECISION_SECONDS, "motion": motion_out, "metrics": metrics}


def load(path: str | Path) -> dict:
    return json.loads(Path(path).read_text())


def step_contexts_from_features(feats: np.ndarray) -> np.ndarray:
    """Each step's context from its own feature columns (mode one-hot, mounted, in_combat): what a bot window
    that carries only its last step's context still says about every step in it."""
    f = np.asarray(feats)
    mode = np.argmax(f[..., motion.INDEX["mode_ground"]:motion.INDEX["mode_airborne"] + 1], axis=-1)
    return motion.context_id(mode, f[..., motion.INDEX["mounted"]], f[..., motion.INDEX["in_combat"]]).astype(
        np.int16)


def realism(reference: dict, windows: np.ndarray) -> dict:
    """Per context and HIST feature, the earth mover's distance between bot steps (every step of every window in
    `windows`, [N, W, F]; overlapping windows count a step once per window they hold it, the same for every step
    of a long track) and the human reference; and the mean over every (context, feature) both sides have."""
    w = np.asarray(windows, dtype=np.float32)
    steps = w.reshape(-1, w.shape[-1]) if w.ndim == 3 else w
    contexts = step_contexts_from_features(steps)
    bot = motion.histograms(steps, contexts)
    out: dict = {"contexts": {}, "mean_emd": float("nan")}
    all_d = []
    for context, per in sorted(bot.items()):
        human = reference.get("motion", {}).get(str(context))
        if human is None:
            continue
        dists = {}
        for name in motion.HIST_FEATURES:
            d = motion.histogram_distance(per[name], np.asarray(human["hist"][name]), motion.HIST_BINS[name])
            dists[name] = None if math.isnan(d) else round(d, 5)
            if not math.isnan(d):
                all_d.append(d)
        valid = [v for v in dists.values() if v is not None]
        out["contexts"][str(context)] = {"name": motion.context_name(context),
                                         "bot_steps": int((contexts == context).sum()),
                                         "human_steps": human.get("steps", 0), "emd": dists,
                                         "mean_emd": round(float(np.mean(valid)), 5) if valid else None}
    if all_d:
        out["mean_emd"] = round(float(np.mean(all_d)), 5)
    return out
