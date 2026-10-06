"""Parity of bots with players on a live realm (player-controller, 2026-10-05): how far the companions' movement --
the player controller, reporting as a client does -- is from the players' own, in one capture where both were
recorded alike (FORMAT.md: one move stream, the same Move fields and clock for both; a session's `kind` 0 is a
player, 1 a companion). It replaces the Playtest procedure: the constants change from what this measures live.

**Contexts.** Every measurement is kept per context -- class and race, map, mode (ground, swim, fly), mounted, in
combat -- and in `all`. A context is compared only where both sides have it; the others are listed as unmatched.

**Sections and their metrics** (each row: players, bots, a divergence, the threshold that counts as a pass, and a
flag -- `pass`, `attention`, or `n/a` where either side has fewer than MIN_SAMPLES):

1. kinematics -- the speed over the speed in force while running, walking, backing, strafing, swimming and flying
   (median; divergence the difference, threshold SPEED_TOLERANCE); the turn and pitch rates between packets, the
   fall heights (quantile distance, DISTRIBUTION_TOLERANCE); jumps, stops and falls a minute (relative difference,
   RATE_TOLERANCE); the highest rise walked onto and the steepest slope walked up (difference, STEP_TOLERANCE yd,
   SLOPE_TOLERANCE degrees).
2. cadence -- the client's time since the last packet before each opcode (median, relative, CADENCE_TOLERANCE),
   the heartbeat's spacing while moving (HEARTBEAT_TOLERANCE ms), the facing turned at a SET_FACING (the 0.1 rad
   rule, FACING_TOLERANCE rad), and changes a minute (RATE_TOLERANCE).
3. realism -- realism.score's EMD of the bots' motion features against a reference made of the players' in the same
   capture, overall and per motion context (REALISM_TOLERANCE).
4. physics -- Replay's calibration measurements (Movement/Replay.cpp), on the players and on the bots, beside the
   controller's constant: the jump and swim-jump launch, the apex, gravity, the air time's error, STEP_UP (highest
   rise walked), the walkable slope, the terminal velocity, the heartbeat, the mouse-look facing threshold, the
   keyboard turn while moving and the vertical share. Divergence |bots - players| / |constant|, PHYSICS_TOLERANCE.
   The float depth needs the liquid's level, which no record holds: not measured.
5. realm timing -- the realm's world tick (MapUpdate records' diffs, per map) and the companions' decision intervals
   (CompanionDecision times, per companion), as quantiles. Training jitter of the tick and the decision interval is
   recommended only past JITTER_TICK_P95_MS (a tick coarser than the forge's own 50 ms) or a decision interval's p5
   or p95 more than JITTER_DECISION_SHARE off the nominal DECISION_MS.

Behaviour metrics (rates, turn and fall distributions, realism) are the policy's; physics and cadence are the
controller's. Nothing here decides anything: a flag is for the user to read.
"""

from __future__ import annotations

import datetime as dt
import json
import math
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from animus.human import motion, realism
from animus.human import reader as r
from animus.human import tracks as tr

# Movement flags (MovementFlags, 3.3.5a) and opcodes (Movement/ReportCadence.h).
FORWARD, BACKWARD, STRAFE_LEFT, STRAFE_RIGHT = 0x1, 0x2, 0x4, 0x8
LEFT, RIGHT, PITCH_UP, PITCH_DOWN = 0x10, 0x20, 0x40, 0x80
WALKING, FALLING, FALLING_FAR = 0x100, 0x1000, 0x2000
ASCENDING, DESCENDING = 0x400000, 0x800000
SWIMMING, FLYING = 0x200000, 0x02000000
AIRBORNE = FALLING | FALLING_FAR
TRANSLATING = FORWARD | BACKWARD | STRAFE_LEFT | STRAFE_RIGHT
HEARTBEAT_FLAGS = 0x00C0100F
OPCODES = {
    0x0B5: "START_FORWARD", 0x0B6: "START_BACKWARD", 0x0B7: "STOP", 0x0B8: "START_STRAFE_LEFT",
    0x0B9: "START_STRAFE_RIGHT", 0x0BA: "STOP_STRAFE", 0x0BB: "JUMP", 0x0BC: "START_TURN_LEFT",
    0x0BD: "START_TURN_RIGHT", 0x0BE: "STOP_TURN", 0x0BF: "START_PITCH_UP", 0x0C0: "START_PITCH_DOWN",
    0x0C1: "STOP_PITCH", 0x0C2: "SET_RUN_MODE", 0x0C3: "SET_WALK_MODE", 0x0C9: "FALL_LAND", 0x0CA: "START_SWIM",
    0x0CB: "STOP_SWIM", 0x0DA: "SET_FACING", 0x0DB: "SET_PITCH", 0x0EE: "HEARTBEAT", 0x359: "START_ASCEND",
    0x35A: "STOP_ASCEND", 0x3A7: "START_DESCEND",
}
OP = {name: code for code, name in OPCODES.items()}
NOT_CHANGES = {OP["HEARTBEAT"], OP["SET_FACING"], OP["SET_PITCH"], OP["FALL_LAND"]}
STOPS = {OP["STOP"], OP["STOP_STRAFE"]}

# The controller's constants (Movement/PlayerController.h, ReportCadence.h).
GRAVITY = 19.2911053
JUMP_SPEED = 7.9555473
SWIM_JUMP_SPEED = 9.0967484
STEP_UP = 1.1917536
TERMINAL_VELOCITY = 60.148003
WALKABLE_DEG = 50.0
HEARTBEAT_MS = 500.0
MOUSE_FACING_THRESHOLD = 0.1
KEYBOARD_TURN_WHILE_MOVING = 0.75
VERTICAL_SHARE = 0.7071068

# Thresholds: what counts as a pass.
MIN_SAMPLES = 5
SPEED_TOLERANCE = 0.03              # the speed over the speed in force, absolute
DISTRIBUTION_TOLERANCE = 0.25       # quantile distance over the players' median magnitude
RATE_TOLERANCE = 0.5                # relative difference of a per-minute rate
STEP_TOLERANCE = 0.15               # yards
SLOPE_TOLERANCE = 3.0               # degrees
CADENCE_TOLERANCE = 0.25            # relative difference of a median interval
HEARTBEAT_TOLERANCE = 25.0          # ms
FACING_TOLERANCE = 0.02             # rad
REALISM_TOLERANCE = 0.15            # realism.score's EMD
PHYSICS_TOLERANCE = 0.02            # of the constant
QUANTILES = np.linspace(0.05, 0.95, 19)
DECISION_MS = 250.0                 # AnimusForge.DecisionMs, the realm's Animus decision interval
JITTER_TICK_P95_MS = 50.0           # a realm tick coarser than the forge's at p95: train with it
JITTER_DECISION_SHARE = 0.2         # decision intervals this far off nominal at p5 or p95: train with it

CLASSES = {1: "warrior", 2: "paladin", 3: "hunter", 4: "rogue", 5: "priest", 6: "deathknight", 7: "shaman",
           8: "mage", 9: "warlock", 11: "druid"}
SIDES = ("players", "bots")


@dataclass
class Side:
    """One side's measurements in one context: named samples and counts."""

    values: dict[str, list[float]] = field(default_factory=dict)
    counts: dict[str, float] = field(default_factory=dict)
    feats: list[np.ndarray] = field(default_factory=list)
    contexts: list[np.ndarray] = field(default_factory=list)

    def add(self, name: str, value: float) -> None:
        if math.isfinite(value):
            self.values.setdefault(name, []).append(float(value))

    def count(self, name: str, n: float = 1.0) -> None:
        self.counts[name] = self.counts.get(name, 0.0) + n

    def get(self, name: str) -> np.ndarray:
        return np.asarray(self.values.get(name, []), dtype=np.float64)


@dataclass
class Study:
    """Every context's two sides."""

    sides: dict[str, dict[str, Side]] = field(default_factory=dict)
    synthesised: int = 0            # companion samples (source 1): kept out of the cadence and physics
    ticks: dict[int, list[np.ndarray]] = field(default_factory=dict)        # map -> MapUpdate diffs (ms)
    decisions: list[np.ndarray] = field(default_factory=list)               # per companion, decision intervals (ms)

    def side(self, context: str, bot: bool) -> Side:
        return self.sides.setdefault(context, {"players": Side(), "bots": Side()})["bots" if bot else "players"]


# Measuring.

def context_key(info: dict, mapid: int, mode: str, mounted: bool, combat: bool) -> str:
    who = f"{CLASSES.get(info.get('class_', 0), 'class' + str(info.get('class_', '?')))}/race{info.get('race', '?')}"
    return f"{who}/map{mapid}/{mode}{'/mounted' if mounted else ''}{'/combat' if combat else ''}"


def packet_mode(flags: int) -> str:
    return "swim" if flags & SWIMMING else "fly" if flags & FLYING else "ground"


def _spans(events: np.ndarray, start: int, end: int) -> list[tuple[float, float]]:
    out, opened = [], None
    for row in events[np.argsort(events["ms"], kind="stable")]:
        if int(row["event"]) == start and opened is None:
            opened = float(row["ms"])
        elif int(row["event"]) == end and opened is not None:
            out.append((opened, float(row["ms"])))
            opened = None
    if opened is not None:
        out.append((opened, math.inf))
    return out


def _speed_in_force(speeds: np.ndarray, ms: float, column: str) -> float:
    if len(speeds) == 0:
        return tr.BASE_SPEEDS.get(column, 0.0)
    before = speeds[speeds["ms"] <= ms]
    row = before[-1] if len(before) else speeds[0]
    return float(row[column])


def measure_packets(study: Study, moves: np.ndarray, events: np.ndarray, speeds: np.ndarray, info: dict, bot: bool,
                    combat: tr.CombatIndex | None = None, player: int = 0) -> None:
    """One mover's packets into the study, Replay's measurements per context: a player's client packets (source 0)
    and a companion's controller packets (source 2); format 1's synthesised companion samples (source 1) are no
    packets and are left out."""
    moves = moves[moves["source"] != r.SOURCE_COMPANION_SAMPLE]
    if len(moves) < 2:
        return
    moves = moves[np.argsort(moves["ms"], kind="stable")]
    speeds = speeds[np.argsort(speeds["ms"], kind="stable")]
    clock = np.where(moves["client_ms"] > 0, moves["client_ms"].astype(np.float64), moves["ms"].astype(np.float64))
    mounted_spans = _spans(events, r.EV_MOUNT, r.EV_DISMOUNT)
    in_combat = combat.in_combat(player, moves["ms"].astype(np.float64) / 1000.0) if combat is not None else None

    def where(k: int) -> list[Side]:
        row = moves[k]
        mounted = any(a <= float(row["ms"]) < b for a, b in mounted_spans)
        fight = bool(in_combat[k]) if in_combat is not None else False
        key = context_key(info, int(row["map"]), packet_mode(int(row["move_flags"])), mounted, fight)
        return [study.side(key, bot), study.side("all", bot)]

    for k in range(1, len(moves)):
        prev, rec = moves[k - 1], moves[k]
        dt_ms = clock[k] - clock[k - 1]
        if dt_ms <= 0 or dt_ms > 1500 or int(prev["map"]) != int(rec["map"]):
            continue
        sides = where(k)
        pf, rf, op = int(prev["move_flags"]), int(rec["move_flags"]), int(rec["opcode"])
        seconds = dt_ms / 1000.0
        dx, dy = float(rec["x"]) - float(prev["x"]), float(rec["y"]) - float(prev["y"])
        rise = float(rec["z"]) - float(prev["z"])
        run = math.hypot(dx, dy)
        name = OPCODES.get(op, f"0x{op:03X}")
        for s in sides:
            s.count("minutes", seconds / 60.0)
            s.add(f"interval {name}", dt_ms)
            if op not in NOT_CHANGES:
                s.count("changes")
            if op in STOPS:
                s.count("stops")

        # Speeds: the keys held over the interval, alone, at the speed in force for them.
        keys = pf & TRANSLATING
        if not (pf & AIRBORNE) and not (rf & AIRBORNE) and dt_ms >= 100:
            mode, column, dist = None, None, run
            if pf & SWIMMING and keys == FORWARD:
                mode, column, dist = "swim", "swim", math.hypot(run, rise)
            elif pf & FLYING and keys == FORWARD:
                mode, column, dist = "fly", "flight", math.hypot(run, rise)
            elif not (pf & (SWIMMING | FLYING)):
                if keys == FORWARD:
                    mode, column = ("walk", "walk") if pf & WALKING else ("run", "run")
                elif keys == BACKWARD and not pf & WALKING:
                    mode, column = "back", "run_back"
                elif keys in (STRAFE_LEFT, STRAFE_RIGHT) and not pf & WALKING:
                    mode, column = "strafe", "run"
            if mode:
                force = _speed_in_force(speeds, float(rec["ms"]), column)
                if force > 0:
                    for s in sides:
                        s.add(f"speed {mode}", dist / seconds / force)

        # Turning and pitching between packets.
        turned = abs(motion.wrap(float(rec["o"]) - float(prev["o"])))
        if dt_ms >= 50 and turned > 1e-4:
            for s in sides:
                s.add("turn rate (rad/s)", turned / seconds)
        pitched = abs(float(rec["pitch"]) - float(prev["pitch"]))
        if dt_ms >= 50 and pitched > 1e-4 and pf & (SWIMMING | FLYING):
            for s in sides:
                s.add("pitch rate (rad/s)", pitched / seconds)

        # Cadence: the heartbeat while moving, the facing at a mouse-look report.
        if op == OP["HEARTBEAT"] and pf & HEARTBEAT_FLAGS:
            for s in sides:
                s.add("HEARTBEAT_MS", dt_ms)
        if op == OP["SET_FACING"]:
            for s in sides:
                s.add("MOUSE_FACING_THRESHOLD", turned)

        # Keyboard turning while moving, over the unit's turn rate.
        turn_keys = pf & (LEFT | RIGHT)
        if turn_keys and turn_keys == rf & (LEFT | RIGHT) and dt_ms >= 100 and pf & TRANSLATING:
            unit = _speed_in_force(speeds, float(rec["ms"]), "turn_rate")
            if unit > 0:
                for s in sides:
                    s.add("KEYBOARD_TURN_WHILE_MOVING", turned / seconds / unit)

        # On foot, no jump: the rises walked onto, the slopes walked up.
        ground = (not (pf & AIRBORNE) and not (rf & AIRBORNE) and op != OP["JUMP"]
                  and int(prev["opcode"]) != OP["FALL_LAND"])
        if ground and not (pf & (SWIMMING | FLYING)):
            if 0.05 < run < 4.0 and rise > 0.3:
                for s in sides:
                    s.add("STEP_UP", rise)
            if run > 1.5 and rise > 0:
                for s in sides:
                    s.add("walkable slope (deg)", math.degrees(math.atan2(rise, run)))

        # Vertical alone, swimming or flying: the vertical speed over the speed in force.
        if pf & (ASCENDING | DESCENDING) and not pf & TRANSLATING and pf & (SWIMMING | FLYING) and dt_ms >= 100:
            force = _speed_in_force(speeds, float(rec["ms"]), "swim" if pf & SWIMMING else "flight")
            if force > 0:
                for s in sides:
                    s.add("VERTICAL_SHARE", abs(rise) / seconds / force)

        # Falling at terminal velocity: past the time gravity takes to reach it.
        if pf & AIRBORNE and rf & AIRBORNE and int(prev["fall_ms"]) > 3300 and dt_ms >= 100:
            for s in sides:
                s.add("TERMINAL_VELOCITY", -rise / seconds)

    # Jumps and falls: a JUMP's launch, and with its landing gravity, the air time and the apex.
    ops = moves["opcode"].astype(np.int64)
    for k in np.flatnonzero(ops == OP["JUMP"]):
        if k == 0:
            continue
        sides = where(int(k))
        swim = int(moves[k - 1]["move_flags"]) & SWIMMING
        v = -float(moves[k]["jump_zspeed"])
        for s in sides:
            s.count("jumps")
            s.add("SWIM_JUMP_SPEED" if swim else "JUMP_SPEED", v)
        if swim:
            continue
        for l in range(k + 1, len(moves)):
            if clock[l] - clock[k] >= 4000:
                break
            if ops[l] != OP["FALL_LAND"]:
                continue
            t = float(moves[l]["fall_ms"]) / 1000.0
            dz = float(moves[l]["z"]) - float(moves[k]["z"])
            if t > 0.1:
                for s in sides:
                    s.add("GRAVITY", 2.0 * (v * t - dz) / (t * t))
                    disc = v * v - 2.0 * GRAVITY * dz
                    if disc >= 0:
                        s.add("jump air time error (ms)", t * 1000.0 - 1000.0 * (v + math.sqrt(disc)) / GRAVITY)
            break
    jumps_at = clock[ops == OP["JUMP"]]
    for l in np.flatnonzero(ops == OP["FALL_LAND"]):
        if np.any((jumps_at <= clock[l]) & (clock[l] - jumps_at < 4000)):
            continue
        before = np.flatnonzero((np.arange(len(moves)) < l) & ((moves["move_flags"].astype(np.int64) & AIRBORNE) == 0))
        if len(before) == 0:
            continue
        height = float(moves[before[-1]]["z"]) - float(moves[l]["z"])
        if height > 0.5:
            for s in where(int(l)):
                s.count("falls")
                s.add("fall height (yd)", height)


def measure_tracks(study: Study, tracks: list[tr.Track]) -> None:
    """The tracks' motion features (motion.features, per motion context) into their sides, for kinematics and
    realism."""
    for track in tracks:
        if len(track.samples) < 2:
            continue
        feats = motion.features(track.samples)
        contexts = motion.step_contexts(track.samples)
        info = track.info or {}
        who = context_key(info, track.map, "", False, False).rsplit("/", 1)[0]
        for key in ("all", who):
            side = study.side(key, track.companion)
            side.feats.append(feats)
            side.contexts.append(contexts)


def study_capture(cap: r.CaptureDir) -> Study:
    study = Study()
    for sessions, shard in tr.iter_shards(cap):
        moves = shard.moves.get(r.MOVE)
        events = shard.moves.get(r.MOTION_EVENT)
        speeds = shard.moves.get(r.SPEEDS)
        study.synthesised += int(np.sum(moves["source"] == r.SOURCE_COMPANION_SAMPLE)) if len(moves) else 0
        for player in np.unique(moves["player"]) if len(moves) else []:
            mine = moves[moves["player"] == player]
            bot = sessions.is_companion(int(player)) or bool(np.isin(
                mine["source"], (r.SOURCE_COMPANION_SAMPLE, r.SOURCE_CONTROLLER)).any())
            measure_packets(study, mine, events[events["player"] == player], speeds[speeds["player"] == player],
                            sessions.info(int(player)), bot, shard.combat, int(player))
        measure_tracks(study, tr.build_tracks(shard.moves, shard.combat, sessions, hour=shard.hour.label,
                                              include_companions=True))
        updates = shard.moves.get(r.MAP_UPDATE)
        for mapid in np.unique(updates["map"]) if len(updates) else []:
            diffs = updates["diff_ms"][updates["map"] == mapid].astype(np.float64)
            study.ticks.setdefault(int(mapid), []).append(diffs)
    decisions = [batch.get(r.COMPANION_DECISION) for _, _, batch in cap.iter_stream("companion")]
    decisions = [d for d in decisions if len(d)]
    if decisions:
        study.decisions.extend(decision_intervals(np.concatenate(decisions)))
    return study


def decision_intervals(decisions: np.ndarray) -> list[np.ndarray]:
    """Each companion's intervals between consecutive decisions (ms), gaps of 5 s or more (a pause) left out."""
    out = []
    for companion in np.unique(decisions["companion"]):
        ms = np.sort(decisions["ms"][decisions["companion"] == companion].astype(np.float64))
        gaps = np.diff(ms)
        gaps = gaps[(gaps > 0) & (gaps < 5000)]
        if len(gaps):
            out.append(gaps)
    return out


def _quantiles(values: np.ndarray) -> dict:
    if not len(values):
        return {"n": 0}
    return {"n": int(len(values)), "p5": float(np.quantile(values, 0.05)), "p50": float(np.median(values)),
            "p95": float(np.quantile(values, 0.95)), "max": float(np.max(values)), "mean": float(np.mean(values))}


def timing(study: Study) -> dict:
    """The realm's tick and decision intervals, and whether they call for training jitter (the thresholds above)."""
    per_map = {str(m): _quantiles(np.concatenate(parts)) for m, parts in sorted(study.ticks.items())}
    ticks = np.concatenate([np.concatenate(p) for p in study.ticks.values()]) if study.ticks else np.zeros(0)
    decisions = np.concatenate(study.decisions) if study.decisions else np.zeros(0)
    tick, decision = _quantiles(ticks), _quantiles(decisions)
    reasons = []
    if tick["n"] and tick["p95"] > JITTER_TICK_P95_MS:
        reasons.append(f"tick p95 {tick['p95']:.0f} ms > {JITTER_TICK_P95_MS:.0f}")
    if decision["n"] and (decision["p95"] > DECISION_MS * (1 + JITTER_DECISION_SHARE)
                          or decision["p5"] < DECISION_MS * (1 - JITTER_DECISION_SHARE)):
        reasons.append(f"decision interval p5-p95 {decision['p5']:.0f}-{decision['p95']:.0f} ms off "
                       f"{DECISION_MS:.0f} by more than {JITTER_DECISION_SHARE:.0%}")
    measured = bool(tick["n"] or decision["n"])
    return {"tick_ms": tick, "tick_ms_by_map": per_map, "decision_ms": decision, "nominal_decision_ms": DECISION_MS,
            "jitter_recommended": bool(reasons) if measured else None, "reasons": reasons,
            "rule": f"jitter if the tick's p95 > {JITTER_TICK_P95_MS:.0f} ms or the decision interval's p5/p95 is "
                    f"more than {JITTER_DECISION_SHARE:.0%} off {DECISION_MS:.0f} ms"}


# Comparing.

def quantile_distance(a: np.ndarray, b: np.ndarray) -> float:
    """The mean gap between the two samples' quantiles (5%..95%), over the first's median magnitude."""
    qa, qb = np.quantile(a, QUANTILES), np.quantile(b, QUANTILES)
    scale = max(abs(float(np.median(a))), 1e-6)
    return float(np.mean(np.abs(qa - qb)) / scale)


def _row(section: str, context: str, metric: str, players, bots, divergence, threshold: float, unit: str = "",
         constant: float | None = None, n: tuple[int, int] = (0, 0), kind: str = "behaviour") -> dict:
    flag = "n/a" if divergence is None or not math.isfinite(divergence) else \
        "pass" if divergence <= threshold else "attention"
    out = {"section": section, "context": context, "metric": metric, "players": players, "bots": bots,
           "divergence": divergence, "threshold": threshold, "flag": flag, "n": list(n), "kind": kind}
    if unit:
        out["unit"] = unit
    if constant is not None:
        out["constant"] = constant
    return out


def _stat(values: np.ndarray, how: str) -> float | None:
    if len(values) < MIN_SAMPLES:
        return None
    return float(np.max(values) if how == "max" else np.median(values) if how == "median" else np.mean(values))


def compare(study: Study) -> list[dict]:
    rows: list[dict] = []
    for context in sorted(study.sides, key=lambda c: (c != "all", c)):
        pl, bt = study.sides[context]["players"], study.sides[context]["bots"]
        if not (pl.counts or pl.feats) or not (bt.counts or bt.feats):
            continue
        n = lambda name: (len(pl.get(name)), len(bt.get(name)))     # noqa: E731

        # Kinematics.
        for mode in ("run", "walk", "back", "strafe", "swim", "fly"):
            name = f"speed {mode}"
            a, b = _stat(pl.get(name), "median"), _stat(bt.get(name), "median")
            if a is None and b is None:
                continue
            rows.append(_row("kinematics", context, f"{mode} speed / speed in force", a, b,
                             abs(b - a) if a is not None and b is not None else None, SPEED_TOLERANCE, n=n(name),
                             kind="controller"))
        for name in ("turn rate (rad/s)", "pitch rate (rad/s)", "fall height (yd)"):
            a, b = pl.get(name), bt.get(name)
            if not len(a) and not len(b):
                continue
            ok = len(a) >= MIN_SAMPLES and len(b) >= MIN_SAMPLES
            rows.append(_row("kinematics", context, f"{name} distribution", _stat(a, "median"), _stat(b, "median"),
                             quantile_distance(a, b) if ok else None, DISTRIBUTION_TOLERANCE, n=n(name)))
        for name in ("jumps", "stops", "falls"):
            ma, mb = pl.counts.get("minutes", 0.0), bt.counts.get("minutes", 0.0)
            if ma <= 0 or mb <= 0:
                continue
            a, b = pl.counts.get(name, 0.0) / ma, bt.counts.get(name, 0.0) / mb
            enough = pl.counts.get(name, 0) + bt.counts.get(name, 0) >= MIN_SAMPLES
            rows.append(_row("kinematics", context, f"{name} a minute", a, b,
                             abs(b - a) / max(a, 1e-6) if enough else None, RATE_TOLERANCE))
        for name, threshold, unit in (("STEP_UP", STEP_TOLERANCE, "yd"),
                                      ("walkable slope (deg)", SLOPE_TOLERANCE, "deg")):
            a, b = _stat(pl.get(name), "max"), _stat(bt.get(name), "max")
            if a is None and b is None:
                continue
            rows.append(_row("kinematics", context, f"highest {name.split(' (')[0].lower()} walked", a, b,
                             abs(b - a) if a is not None and b is not None else None, threshold, unit, n=n(name),
                             kind="controller"))

        # Cadence.
        for name in sorted(k for k in set(pl.values) | set(bt.values) if k.startswith("interval ")):
            a, b = _stat(pl.get(name), "median"), _stat(bt.get(name), "median")
            if a is None or b is None:
                continue
            rows.append(_row("cadence", context, f"ms before {name[9:]}", a, b, abs(b - a) / max(a, 1e-6),
                             CADENCE_TOLERANCE, "ms", n=n(name), kind="controller"))
        for name, threshold, unit in (("HEARTBEAT_MS", HEARTBEAT_TOLERANCE, "ms"),
                                      ("MOUSE_FACING_THRESHOLD", FACING_TOLERANCE, "rad")):
            a, b = _stat(pl.get(name), "median"), _stat(bt.get(name), "median")
            if a is None and b is None:
                continue
            rows.append(_row("cadence", context, f"{name} (median)", a, b,
                             abs(b - a) if a is not None and b is not None else None, threshold, unit, n=n(name),
                             kind="controller"))
        ma, mb = pl.counts.get("minutes", 0.0), bt.counts.get("minutes", 0.0)
        if ma > 0 and mb > 0:
            a, b = pl.counts.get("changes", 0.0) / ma, bt.counts.get("changes", 0.0) / mb
            rows.append(_row("cadence", context, "changes a minute", a, b, abs(b - a) / max(a, 1e-6), RATE_TOLERANCE))

        # Realism: the bots' motion against the players' in the same context.
        if pl.feats and bt.feats:
            pf, pc = np.concatenate(pl.feats), np.concatenate(pl.contexts)
            bf, bc = np.concatenate(bt.feats), np.concatenate(bt.contexts)
            reference = {"motion": {c: {"hist": h} for c, h in motion.histograms(pf, pc).items()}}
            scored = realism.score(bf, bc, reference)
            rows.append(_row("realism", context, "realism EMD (all motion)", None, scored["realism_emd"],
                             scored["realism_emd"], REALISM_TOLERANCE, n=(len(pc), len(bc))))
            for name in sorted(scored["features"]):
                value = scored.get(f"realism_emd_{name}", float("nan"))
                rows.append(_row("realism", context, f"realism EMD ({name})", None, value, value, REALISM_TOLERANCE,
                                 n=(int(np.sum(pc == _context_index(name))), scored["steps"].get(name, 0))))

        # Physics constants.
        for name, constant, how, note in PHYSICS:
            a, b = _physics(pl, name, how), _physics(bt, name, how)
            if a is None and b is None:
                continue
            scale = abs(constant) if constant else 1.0
            rows.append(_row("physics", context, name, a, b,
                             abs(b - a) / scale if a is not None and b is not None else None, PHYSICS_TOLERANCE,
                             constant=constant, n=n(name if name != "jump apex (yd)" else "JUMP_SPEED"),
                             kind="controller"))
    return rows


def _context_index(name: str) -> int:
    return next((c for c in range(motion.CONTEXTS) if motion.context_name(c) == name), -1)


# (name, the controller's constant, how a side's value is read, what it is)
PHYSICS = (
    ("JUMP_SPEED", JUMP_SPEED, "mean", "the JUMP packets' launch"),
    ("SWIM_JUMP_SPEED", SWIM_JUMP_SPEED, "mean", "jumps from swimming"),
    ("jump apex (yd)", JUMP_SPEED ** 2 / (2 * GRAVITY), "apex", "v^2 / 2g of the launch measured"),
    ("GRAVITY", GRAVITY, "mean", "each jump's launch, fall time and drop"),
    ("jump air time error (ms)", 0.0, "mean", "landed minus predicted"),
    ("STEP_UP", STEP_UP, "max", "the highest rise walked onto without a jump (a lower bound)"),
    ("walkable slope (deg)", WALKABLE_DEG, "max", "the steepest slope walked up over 1.5 yd or more"),
    ("TERMINAL_VELOCITY", TERMINAL_VELOCITY, "median", "falling past 3.3 s"),
    ("HEARTBEAT_MS", HEARTBEAT_MS, "median", "a heartbeat after the last packet, while moving"),
    ("MOUSE_FACING_THRESHOLD", MOUSE_FACING_THRESHOLD, "median", "the facing turned at a SET_FACING"),
    ("KEYBOARD_TURN_WHILE_MOVING", KEYBOARD_TURN_WHILE_MOVING, "median", "keyboard turn moving, over the unit's"),
    ("VERTICAL_SHARE", VERTICAL_SHARE, "median", "ascending or descending alone, over the speed"),
)


def _physics(side: Side, name: str, how: str) -> float | None:
    if how == "apex":
        launch = _stat(side.get("JUMP_SPEED"), "mean")
        return None if launch is None else launch * launch / (2 * GRAVITY)
    return _stat(side.get(name), how)


def report(study: Study, source: str = "") -> dict:
    rows = compare(study)
    unmatched = {context: {s: bool(sides[s].counts or sides[s].feats) for s in SIDES}
                 for context, sides in study.sides.items()
                 if not all(sides[s].counts or sides[s].feats for s in SIDES)}
    flags = {flag: sum(1 for row in rows if row["flag"] == flag) for flag in ("pass", "attention", "n/a")}
    return {"format": 1, "built": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"), "source": source,
            "thresholds": {"min_samples": MIN_SAMPLES, "speed": SPEED_TOLERANCE,
                           "distribution": DISTRIBUTION_TOLERANCE, "rate": RATE_TOLERANCE, "step_yd": STEP_TOLERANCE,
                           "slope_deg": SLOPE_TOLERANCE, "cadence": CADENCE_TOLERANCE,
                           "heartbeat_ms": HEARTBEAT_TOLERANCE, "facing_rad": FACING_TOLERANCE,
                           "realism_emd": REALISM_TOLERANCE, "physics": PHYSICS_TOLERANCE},
            "not_measured": {"FLOAT_DEPTH": "needs the liquid's level, which no capture record holds"},
            "synthesised_companion_samples": study.synthesised,
            "summary": flags, "timing": timing(study),
            "attention": [row for row in rows if row["flag"] == "attention"],
            "rows": rows, "unmatched_contexts": unmatched}


def _fmt(value) -> str:
    if value is None:
        return "-"
    if isinstance(value, float):
        return "nan" if not math.isfinite(value) else f"{value:.4g}"
    return str(value)


def markdown(rep: dict) -> str:
    lines = [f"# Bot / player parity ({rep['source']})", "",
             f"{rep['summary']['pass']} pass, {rep['summary']['attention']} attention, {rep['summary']['n/a']} n/a "
             f"(fewer than {rep['thresholds']['min_samples']} samples a side). Behaviour rows are the policy's; "
             "controller rows (speeds, steps, cadence, physics) are the controller's. A flag is for reading, not a "
             "decision.", ""]
    for section in ("kinematics", "cadence", "realism", "physics"):
        rows = [row for row in rep["rows"] if row["section"] == section]
        if not rows:
            continue
        lines += [f"## {section}", "", "| flag | context | metric | players | bots | constant | divergence | "
                  "threshold | n (players, bots) |", "|---|---|---|---|---|---|---|---|---|"]
        for row in rows:
            lines.append(f"| {row['flag']} | {row['context']} | {row['metric']} | {_fmt(row['players'])} | "
                         f"{_fmt(row['bots'])} | {_fmt(row.get('constant'))} | {_fmt(row['divergence'])} | "
                         f"{_fmt(row['threshold'])} | {row['n'][0]}, {row['n'][1]} |")
        lines.append("")
    if rep["unmatched_contexts"]:
        lines += ["## Unmatched contexts", ""]
        lines += [f"- {context}: " + ", ".join(s for s, has in sides.items() if has) + " only"
                  for context, sides in sorted(rep["unmatched_contexts"].items())]
        lines.append("")
    t = rep["timing"]
    def q(d: dict) -> str:
        if not d.get("n"):
            return "not in the capture"
        return f"p5 {d['p5']:.1f}, p50 {d['p50']:.1f}, p95 {d['p95']:.1f}, max {d['max']:.1f} ms (n {d['n']})"

    verdict = {None: "not measured", True: "recommended (" + "; ".join(t["reasons"]) + ")",
               False: "not needed"}[t["jitter_recommended"]]
    lines += ["## realm timing", "", f"- world tick (MapUpdate): {q(t['tick_ms'])}",
              f"- decision interval (CompanionDecision): {q(t['decision_ms'])}",
              f"- training jitter: {verdict}; rule: {t['rule']}", ""]
    lines += ["Not measured: " + "; ".join(f"{k} ({v})" for k, v in rep["not_measured"].items()), ""]
    return "\n".join(lines)


def write(rep: dict, out: str | Path) -> tuple[Path, Path]:
    out = Path(out)
    stem = out.with_suffix("") if out.suffix in (".json", ".md") else out / "parity"
    stem.parent.mkdir(parents=True, exist_ok=True)
    js, md = stem.with_suffix(".json"), stem.with_suffix(".md")
    js.write_text(json.dumps(rep, indent=1, default=float))
    md.write_text(markdown(rep))
    return js, md
