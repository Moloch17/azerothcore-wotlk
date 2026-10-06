"""Parity of Animus companions with human players on a live realm (player-controller, 2026-10-05; the definitions are
.agents/plans/player-controller/parity.METRICS.md, approved by the overseer with the statistics below). It replaces
the Playtest: the controller's constants change from what this measures live -- proposed here, decided by the user.

**Data.** One capture, capture format 3: players' client packets (Move `source` 0) and companions' controller packets
(`source` 2) in one move stream, the same fields; format 1's synthesised companion samples (`source` 1) are no
packets and are left out. Intervals and cadence are on the server's clock (`server_ms`, the movement handler's
getMSTime): a player's trails the packet's arrival by its session-queue wait (up to one update), a companion's has
none -- read the per-opcode interval rows with that in mind. A file without `server_ms` falls back to `client_ms`,
and the report says which clock it read. Cutting spans (taxi, teleport, death, loading, vehicle, transport) and
involuntary ones (root, stun, fear, knockback) are left out.

**Strata.** Every packet's state from MoverState (class, level band, on foot or mounted, form, zone; the mode from the
packet's flags; in combat), carried forward; a file with none falls back to the session's class and zone, the mount
events and the combat index. A companion's rows are split by its model and move revision. A stratum too small for a
metric pools up -- zone, then level band, then form, then class -- and its row names the stratum it was read in;
`all` is always reported.

**Rows.** players, bots, the divergence with its 95% interval, the threshold, n (samples and sessions a side), and:
`attention` when the interval lies wholly above the threshold, `pass` when wholly at or below it, `inconclusive`
otherwise or below the minimum (30 samples and 3 sessions a side for distributions, 10 samples and 2 sessions for
rates and medians -- a bootstrap by session needs two). The interval is a bootstrap of BOOTSTRAP resamples by
session, each side resampled on its own. Per section the rows are tested together: each row's one-sided p (the
share of resamples at or below the threshold) through Benjamini-Hochberg at FDR; `survives` marks the attentions left,
and the section reports its attentions against the number expected by chance. The headline is the `all` stratum and
the surviving attentions.

**Kinds.** `controller` rows (speeds, cadence, physics, refusals) test the controller and the client's rules: an
attention there is a controller or constant question. `behaviour` rows (turning, starts and stops, run lengths,
jumps and falls a minute, acceleration, realism) are the policy's choices: they inform training -- style and the
human-likeness reward -- and never gate a controller change.

**Normalisation** (each row's `norm`): `abs` in the metric's own unit; `median` over the players' median, only for
strictly positive quantities (fall heights, run lengths, |turn rate|, |pitch rate|): turn and pitch rates are compared
as magnitudes, since their sign is symmetric, so no signed, zero-centred quantity is divided by its median (the signed
acceleration is an EMD on fixed bins); `relative` for rates (over the players' rate); `constant` over the
controller's constant.

**Exposure.** The highest rise walked and the steepest slope walked depend on the terrain met, not on the physics:
they are calibration only -- each side against the constant (an upper check: walking higher or steeper than the
constant allows is the attention), never bots against players. Steep descents need the faces met, which no packet
says: reported, not flagged.
"""

from __future__ import annotations

import datetime as dt
import json
import math
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from animus.human import motion, reference
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
STARTS = {OP["START_FORWARD"], OP["START_BACKWARD"], OP["START_STRAFE_LEFT"], OP["START_STRAFE_RIGHT"]}
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

# Statistics.
BOOTSTRAP = 1000
CONFIDENCE = 0.95
FDR = 0.1
MIN_DIST_SAMPLES, MIN_DIST_SESSIONS = 30, 3
MIN_STAT_SAMPLES, MIN_STAT_SESSIONS = 10, 2
QUANTILES = np.linspace(0.05, 0.95, 19)
CONSTANT_CHANGE_N = 30              # players' samples before a constant change is proposed

# Strata: the dimensions, and the order they pool up in.
DIMS = ("class", "band", "mount", "form", "zone", "mode", "combat")
POOL = ("zone", "band", "form", "class")
ALL = tuple("*" for _ in DIMS)
CLASSES = {1: "warrior", 2: "paladin", 3: "hunter", 4: "rogue", 5: "priest", 6: "deathknight", 7: "shaman",
           8: "mage", 9: "warlock", 11: "druid"}

# Realm timing (FORMAT.md MapUpdate, CompanionDecision).
DECISION_MS = 250.0
JITTER_TICK_P95_MS = 50.0
JITTER_DECISION_SHARE = 0.2
SECTIONS = ("refusals", "kinematics", "cadence", "realism", "physics")


@dataclass(frozen=True)
class Spec:
    """One metric: its section, kind, how its divergence is read, the threshold that passes, and its normalisation."""

    name: str
    section: str
    kind: str                       # controller / behaviour
    stat: str                       # median_diff, qdist, rate, emd, refusals, parity, calibration
    threshold: float
    norm: str
    unit: str = ""
    source: str = ""                # the observations read (default: the name)
    constant: float | None = None
    how: str = "median"             # a side's value: median, mean, max, p1, apex (physics and calibration)
    side: str = ""                  # calibration: the side measured against the constant
    upper: bool = False             # calibration of an exposure-dependent maximum: only above the constant counts

    @property
    def distributional(self) -> bool:
        return self.stat in ("qdist", "emd")

    @property
    def metric(self) -> str:
        return self.source or self.name


SPEED_MODES = ("run", "walk", "back", "strafe", "diagonal", "swim", "fly")
SPECS: list[Spec] = [Spec("refused per 1000 packets", "refusals", "controller", "refusals", 1.0, "abs",
                          "per 1000")]
for _mode in SPEED_MODES:
    SPECS.append(Spec(f"{_mode} speed / speed in force (median)", "kinematics", "controller", "median_diff", 0.03,
                      "abs", source=f"{_mode} speed"))
    SPECS.append(Spec(f"{_mode} speed / speed in force (distribution)", "kinematics", "controller", "qdist", 0.10,
                      "abs", source=f"{_mode} speed"))
SPECS += [
    Spec("|turn rate| (rad/s)", "kinematics", "behaviour", "qdist", 0.25, "median"),
    Spec("|pitch rate| (rad/s)", "kinematics", "behaviour", "qdist", 0.25, "median"),
    Spec("starts and stops a minute", "kinematics", "behaviour", "rate", 0.5, "relative"),
    Spec("run length (s)", "kinematics", "behaviour", "qdist", 0.25, "median"),
    Spec("acceleration (motion accel EMD)", "kinematics", "behaviour", "emd", 0.15, "abs"),
    Spec("jumps a minute", "kinematics", "behaviour", "rate", 0.5, "relative"),
    Spec("falls a minute", "kinematics", "behaviour", "rate", 0.5, "relative"),
    Spec("fall height (yd)", "kinematics", "behaviour", "qdist", 0.25, "median"),
    Spec("HEARTBEAT spacing while moving", "cadence", "controller", "median_diff", 25.0, "abs", "ms",
         source="HEARTBEAT_MS"),
    Spec("facing turned at SET_FACING", "cadence", "controller", "median_diff", 0.02, "abs", "rad",
         source="MOUSE_FACING_THRESHOLD"),
    Spec("keyboard turn while moving / unit rate", "cadence", "controller", "median_diff", 0.05, "abs",
         source="KEYBOARD_TURN_WHILE_MOVING"),
    Spec("changes a minute", "cadence", "behaviour", "rate", 0.5, "relative"),
    Spec("reports a minute", "cadence", "behaviour", "rate", 0.5, "relative"),
    Spec("realism EMD", "realism", "behaviour", "emd", 0.15, "abs"),
]
# Physics: Replay's measurements (Movement/Replay.cpp), bots against players and each side against the constant.
# (name, the observations read, constant, how a side's value is read, unit, threshold, norm, exposure-dependent)
PHYSICS = (
    ("JUMP_SPEED", "JUMP_SPEED", JUMP_SPEED, "mean", "yd/s", 0.02, "constant", False),
    ("SWIM_JUMP_SPEED", "SWIM_JUMP_SPEED", SWIM_JUMP_SPEED, "mean", "yd/s", 0.02, "constant", False),
    ("jump apex", "JUMP_SPEED", JUMP_SPEED ** 2 / (2 * GRAVITY), "apex", "yd", 0.02, "constant", False),
    ("GRAVITY", "GRAVITY", GRAVITY, "mean", "yd/s^2", 0.02, "constant", False),
    ("jump air time error", "jump air time error", 0.0, "mean", "ms", 20.0, "abs", False),
    ("TERMINAL_VELOCITY", "TERMINAL_VELOCITY", TERMINAL_VELOCITY, "median", "yd/s", 0.02, "constant", False),
    ("HEARTBEAT_MS", "HEARTBEAT_MS", HEARTBEAT_MS, "median", "ms", 0.02, "constant", False),
    # Read at the steps' 1st percentile: every step is at or past the rule (a client is frame-limited, so it lands a
    # little past it), so the rule is the distribution's lower edge -- the percentile, not the minimum, for a stray.
    ("MOUSE_FACING_THRESHOLD", "MOUSE_FACING_THRESHOLD", MOUSE_FACING_THRESHOLD, "p1", "rad", 0.02, "constant",
     False),
    ("KEYBOARD_TURN_WHILE_MOVING", "KEYBOARD_TURN_WHILE_MOVING", KEYBOARD_TURN_WHILE_MOVING, "median", "", 0.02,
     "constant", False),
    ("VERTICAL_SHARE", "VERTICAL_SHARE", VERTICAL_SHARE, "median", "", 0.02, "constant", False),
    ("STEP_UP (highest rise walked)", "rise walked", STEP_UP, "max", "yd", 0.02, "constant", True),
    ("walkable slope (steepest walked up)", "slope walked", WALKABLE_DEG, "max", "deg", 0.02, "constant", True),
)
for _name, _source, _constant, _how, _unit, _threshold, _norm, _exposure in PHYSICS:
    if not _exposure:
        SPECS.append(Spec(_name, "physics", "controller", "parity", _threshold, _norm, _unit, _source, _constant,
                          _how))
    for _side in ("players", "bots"):
        SPECS.append(Spec(f"{_name} ({_side} vs constant)", "physics", "controller", "calibration", _threshold,
                          _norm, _unit, _source, _constant, _how, _side, _exposure))
# Measured, never flagged: what no threshold can be set for from packets alone.
REPORTED = ("steep descents walked (per 1000 descending runs)", "network share (server - client interval, ms)")


# Observations.

@dataclass
class Observations:
    """Every measurement: its metric, side, companion model, session, stratum and value. An event carries 1, and its
    exposure (minutes moving) is the metric "minutes"."""

    metric: list[str] = field(default_factory=list)
    bot: list[bool] = field(default_factory=list)
    model: list[str] = field(default_factory=list)
    session: list[int] = field(default_factory=list)
    key: list[tuple] = field(default_factory=list)
    value: list[float] = field(default_factory=list)

    def add(self, metric: str, bot: bool, model: str, session: int, key: tuple, value: float) -> None:
        if math.isfinite(value):
            self.metric.append(metric)
            self.bot.append(bot)
            self.model.append(model)
            self.session.append(session)
            self.key.append(key)
            self.value.append(float(value))


@dataclass
class Study:
    obs: Observations = field(default_factory=Observations)
    hists: dict = field(default_factory=dict)       # (bot, model, session, class or *) -> {context: {feature: counts}}
    ticks: dict[int, list[np.ndarray]] = field(default_factory=dict)
    decisions: list[np.ndarray] = field(default_factory=list)
    clocks: dict[str, int] = field(default_factory=dict)
    synthesised: int = 0


@dataclass
class MoverStates:
    """A mover's MoverState records, sorted by time: the state in force at any time."""

    ms: np.ndarray
    rows: np.ndarray

    def at(self, ms: np.ndarray) -> np.ndarray:
        return np.clip(np.searchsorted(self.ms, ms, side="right") - 1, 0, len(self.ms) - 1)


def _band(level: int) -> str:
    return reference.level_band(int(level)) if level else "?"


def _mode(flags: int) -> str:
    return "swim" if flags & SWIMMING else "fly" if flags & FLYING else "airborne" if flags & AIRBORNE else "ground"


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


def _in(spans: list[tuple[float, float]], t: np.ndarray) -> np.ndarray:
    out = np.zeros(len(t), dtype=bool)
    for a, b in spans:
        out |= (t >= a) & (t < b)
    return out


def _speed_in_force(speeds: np.ndarray, ms: float, column: str) -> float:
    if len(speeds) == 0:
        return tr.BASE_SPEEDS.get(column, 0.0)
    before = speeds[speeds["ms"] <= ms]
    row = before[-1] if len(before) else speeds[0]
    return float(row[column])


def _clock(moves: np.ndarray) -> tuple[np.ndarray, str]:
    """Each packet's time (ms) on the clock intervals are read on: the server's when the file has it, else the
    client's, else the server's unix time. A 32-bit clock's wrap is unrolled."""
    for column, name in (("server_ms", "server"), ("client_ms", "client")):
        if column in (moves.dtype.names or ()) and np.all(moves[column] > 0):
            raw = moves[column].astype(np.int64)
            steps = np.diff(raw) % (1 << 32)
            steps = np.where(steps > (1 << 31), steps - (1 << 32), steps)
            return np.concatenate([[0], np.cumsum(steps)]).astype(np.float64) + float(raw[0]), name
    return moves["ms"].astype(np.float64), "unix"


def measure_mover(study: Study, moves: np.ndarray, events: np.ndarray, speeds: np.ndarray, states: MoverStates | None,
                  info: dict, bot: bool, model: str, session: int, combat: tr.CombatIndex | None = None,
                  player: int = 0) -> None:
    """One mover's packets into the study."""
    moves = moves[moves["source"] != r.SOURCE_COMPANION_SAMPLE]
    if len(moves) < 2:
        return
    moves = moves[np.argsort(moves["ms"], kind="stable")]
    speeds = speeds[np.argsort(speeds["ms"], kind="stable")]
    clock, clock_name = _clock(moves)
    study.clocks[clock_name] = study.clocks.get(clock_name, 0) + len(moves)
    client = moves["client_ms"].astype(np.float64)
    unix = moves["ms"].astype(np.float64)
    flags_all = moves["move_flags"].astype(np.int64)
    ops = moves["opcode"].astype(np.int64)

    # Left out: cutting and involuntary spans, transports.
    cut = []
    for a, b in ((r.EV_TAXI_START, r.EV_TAXI_END), (r.EV_DEATH, r.EV_RESURRECT),
                 (r.EV_LOADING_START, r.EV_LOADING_END), (r.EV_VEHICLE_ENTER, r.EV_VEHICLE_EXIT),
                 (r.EV_TRANSPORT_BOARD, r.EV_TRANSPORT_LEAVE), (r.EV_ROOT, r.EV_UNROOT),
                 (r.EV_STUN_START, r.EV_STUN_END), (r.EV_FEAR_START, r.EV_FEAR_END)):
        cut += _spans(events, a, b)
    for tk in events["ms"][events["event"] == r.EV_KNOCKBACK].astype(np.float64):
        cut.append((tk, tk + tr.KNOCKBACK_MAX * 1000.0))
    for tk in events["ms"][events["event"] == r.EV_TELEPORT].astype(np.float64):
        cut.append((tk - 1.0, tk + 1.0))
    skip = _in(cut, unix) | ((flags_all & tr.MF_ONTRANSPORT) != 0)

    # Each packet's stratum: MoverState, or the session and the events.
    n = len(moves)
    if states is not None and len(states.ms):
        rows = states.rows[states.at(unix)]
        cls = [CLASSES.get(int(c), f"class{int(c)}") for c in rows["class_"]]
        band = [_band(int(v)) for v in rows["level"]]
        mount = ["mounted" if int(m) else "foot" for m in rows["mount"]]
        form = [str(int(f)) for f in rows["form"]]
        zone = [str(int(z)) for z in rows["zone"]]
        fighting = rows["in_combat"].astype(bool)
    else:
        cls = [CLASSES.get(info.get("class_", 0), f"class{info.get('class_', '?')}")] * n
        band = [_band(info.get("level", 0))] * n
        mount = ["mounted" if m else "foot" for m in _in(_spans(events, r.EV_MOUNT, r.EV_DISMOUNT), unix)]
        form = ["0"] * n
        zone = [str(info.get("zone", 0))] * n
        fighting = combat.in_combat(player, unix / 1000.0) if combat is not None else np.zeros(n, dtype=bool)

    def key(k: int) -> tuple:
        return (cls[k], band[k], mount[k], form[k], zone[k], _mode(int(flags_all[k])), str(int(fighting[k])))

    def add(metric: str, k: int, value: float) -> None:
        study.obs.add(metric, bot, model, session, key(k), value)

    run_started: float | None = None
    descents = steep = 0
    for k in range(1, n):
        if skip[k] or skip[k - 1] or int(moves[k]["map"]) != int(moves[k - 1]["map"]):
            run_started = None
            continue
        dt_ms = clock[k] - clock[k - 1]
        if dt_ms <= 0 or dt_ms > 1500:
            run_started = None
            continue
        prev, rec = moves[k - 1], moves[k]
        pf, rf, op = int(prev["move_flags"]), int(rec["move_flags"]), int(ops[k])
        seconds = dt_ms / 1000.0
        dx, dy = float(rec["x"]) - float(prev["x"]), float(rec["y"]) - float(prev["y"])
        rise = float(rec["z"]) - float(prev["z"])
        run = math.hypot(dx, dy)
        moving = bool(pf & (TRANSLATING | AIRBORNE | ASCENDING | DESCENDING))

        if clock_name != "client" and client[k] > 0 and client[k - 1] > 0:
            add("network share (server - client interval, ms)", k, dt_ms - (client[k] - client[k - 1]))
        if moving:
            add("minutes", k, seconds / 60.0)
            add("reports a minute", k, 1.0)
            if op not in NOT_CHANGES:
                add("changes a minute", k, 1.0)
        if op in STARTS or op in STOPS:
            add("starts and stops a minute", k, 1.0)
        if op in STARTS and run_started is None:
            run_started = clock[k]
        elif op in STOPS and run_started is not None:
            add("run length (s)", k, (clock[k] - run_started) / 1000.0)
            run_started = None

        # Speeds: the keys held over the interval, at the speed in force for them.
        keys = pf & TRANSLATING
        if not (pf & AIRBORNE) and not (rf & AIRBORNE) and dt_ms >= 100:
            mode, column, dist = None, None, run
            if pf & SWIMMING and keys == FORWARD:
                mode, column, dist = "swim", "swim", math.hypot(run, rise)
            elif pf & FLYING and keys == FORWARD:
                mode, column, dist = "fly", "flight", math.hypot(run, rise)
            elif not (pf & (SWIMMING | FLYING)):
                walking = bool(pf & WALKING)
                if keys == FORWARD:
                    mode, column = ("walk", "walk") if walking else ("run", "run")
                elif keys == BACKWARD and not walking:
                    mode, column = "back", "run_back"
                elif keys in (STRAFE_LEFT, STRAFE_RIGHT) and not walking:
                    mode, column = "strafe", "run"
                elif keys in (FORWARD | STRAFE_LEFT, FORWARD | STRAFE_RIGHT) and not walking:
                    mode, column = "diagonal", "run"
            if mode:
                force = _speed_in_force(speeds, float(rec["ms"]), column)
                if force > 0:
                    add(f"{mode} speed", k, dist / seconds / force)

        # Turning and pitching, as magnitudes.
        turned = abs(motion.wrap(float(rec["o"]) - float(prev["o"])))
        if dt_ms >= 50 and turned > 1e-4:
            add("|turn rate| (rad/s)", k, turned / seconds)
        pitched = abs(float(rec["pitch"]) - float(prev["pitch"]))
        if dt_ms >= 50 and pitched > 1e-4 and pf & (SWIMMING | FLYING):
            add("|pitch rate| (rad/s)", k, pitched / seconds)

        # Cadence (and its physics constants).
        if op == OP["HEARTBEAT"] and pf & HEARTBEAT_FLAGS:
            add("HEARTBEAT_MS", k, dt_ms)
        if op == OP["SET_FACING"]:
            add("MOUSE_FACING_THRESHOLD", k, turned)
        turn_keys = pf & (LEFT | RIGHT)
        if turn_keys and turn_keys == rf & (LEFT | RIGHT) and dt_ms >= 100 and pf & TRANSLATING:
            unit = _speed_in_force(speeds, float(rec["ms"]), "turn_rate")
            if unit > 0:
                add("KEYBOARD_TURN_WHILE_MOVING", k, turned / seconds / unit)

        # On foot, no jump: rises walked onto, slopes walked up and down.
        ground = (not (pf & AIRBORNE) and not (rf & AIRBORNE) and op != OP["JUMP"]
                  and int(prev["opcode"]) != OP["FALL_LAND"] and not (pf & (SWIMMING | FLYING)))
        if ground:
            if 0.05 < run < 4.0 and rise > 0.3:
                add("rise walked", k, rise)
            if run > 1.5 and rise > 0:
                add("slope walked", k, math.degrees(math.atan2(rise, run)))
            if run > 1.5 and rise < 0:
                descents += 1
                steep += -rise / run > math.tan(math.radians(WALKABLE_DEG))

        if pf & (ASCENDING | DESCENDING) and not pf & TRANSLATING and pf & (SWIMMING | FLYING) and dt_ms >= 100:
            force = _speed_in_force(speeds, float(rec["ms"]), "swim" if pf & SWIMMING else "flight")
            if force > 0:
                add("VERTICAL_SHARE", k, abs(rise) / seconds / force)
        if pf & AIRBORNE and rf & AIRBORNE and int(prev["fall_ms"]) > 3300 and dt_ms >= 100:
            add("TERMINAL_VELOCITY", k, -rise / seconds)
    if descents:
        add("steep descents walked (per 1000 descending runs)", n - 1, 1000.0 * steep / descents)

    # Jumps (in the stratum they were pressed in: the packet before) and their landings; falls with no jump.
    for k in np.flatnonzero(ops == OP["JUMP"]):
        if k == 0 or skip[k]:
            continue
        k = int(k)
        swim = int(moves[k - 1]["move_flags"]) & SWIMMING
        v = -float(moves[k]["jump_zspeed"])
        study.obs.add("jumps a minute", bot, model, session, key(k - 1), 1.0)
        study.obs.add("SWIM_JUMP_SPEED" if swim else "JUMP_SPEED", bot, model, session, key(k - 1), v)
        if swim:
            continue
        for land in range(k + 1, n):
            if clock[land] - clock[k] >= 4000:
                break
            if ops[land] != OP["FALL_LAND"]:
                continue
            t = float(moves[land]["fall_ms"]) / 1000.0
            dz = float(moves[land]["z"]) - float(moves[k]["z"])
            if t > 0.1:
                study.obs.add("GRAVITY", bot, model, session, key(k - 1), 2.0 * (v * t - dz) / (t * t))
                disc = v * v - 2.0 * GRAVITY * dz
                if disc >= 0:
                    study.obs.add("jump air time error", bot, model, session, key(k - 1),
                                  t * 1000.0 - 1000.0 * (v + math.sqrt(disc)) / GRAVITY)
            break
    jumps_at = clock[ops == OP["JUMP"]]
    grounded = np.flatnonzero((flags_all & AIRBORNE) == 0)
    for land in np.flatnonzero(ops == OP["FALL_LAND"]):
        if skip[land] or np.any((jumps_at <= clock[land]) & (clock[land] - jumps_at < 4000)):
            continue
        before = grounded[grounded < land]
        if len(before) == 0:
            continue
        height = float(moves[before[-1]]["z"]) - float(moves[land]["z"])
        if height > 0.5:
            add("falls a minute", int(land), 1.0)
            add("fall height (yd)", int(land), height)


def measure_tracks(study: Study, tracks: list[tr.Track], who: dict[int, tuple[bool, str, int, str]]) -> None:
    """The tracks' motion histograms (motion.features on the decision grid) per session, by class and in all, for the
    realism and acceleration rows."""
    for track in tracks:
        if len(track.samples) < 2 or track.player not in who:
            continue
        bot, model, session, cls = who[track.player]
        feats = motion.features(track.samples)
        contexts = motion.step_contexts(track.samples)
        for context, hists in motion.histograms(feats, contexts).items():
            for slot_class in (cls, "*"):
                slot = study.hists.setdefault((bot, model, session, slot_class), {}).setdefault(context, {})
                for feature, counts in hists.items():
                    slot[feature] = slot.get(feature, 0) + np.asarray(counts, dtype=np.float64)


def _states(batch: r.Batch) -> dict[int, MoverStates]:
    rows = batch.get(r.MOVER_STATE)
    out = {}
    for player in np.unique(rows["player"]) if len(rows) else []:
        mine = rows[rows["player"] == player]
        mine = mine[np.argsort(mine["ms"], kind="stable")]
        out[int(player)] = MoverStates(mine["ms"].astype(np.float64), mine)
    return out


def study_capture(cap: r.CaptureDir) -> Study:
    study = Study()
    tallies: list[np.void] = []
    who: dict[int, tuple[bool, str, int, str]] = {}
    for sessions, shard in tr.iter_shards(cap):
        moves = shard.moves.get(r.MOVE)
        events = shard.moves.get(r.MOTION_EVENT)
        speeds = shard.moves.get(r.SPEEDS)
        states = _states(shard.moves)
        study.synthesised += int(np.sum(moves["source"] == r.SOURCE_COMPANION_SAMPLE)) if len(moves) else 0
        for player in np.unique(moves["player"]) if len(moves) else []:
            player = int(player)
            mine = moves[moves["player"] == player]
            info = sessions.info(player)
            state = states.get(player)
            last = state.rows[-1] if state is not None else None
            bot = sessions.is_companion(player) or (last is not None and int(last["kind"]) == 1) or bool(
                np.isin(mine["source"], (r.SOURCE_COMPANION_SAMPLE, r.SOURCE_CONTROLLER)).any())
            model = ""
            if bot:
                name = bytes(last["model"]).rstrip(b"\0").decode(errors="replace") if last is not None else ""
                model = f"{name or 'companion'} r{int(last['move_revision'])}" if last is not None else "companion"
            session = int(info.get("session", 0)) or player
            cls = CLASSES.get(int(last["class_"]) if last is not None else int(info.get("class_", 0)), "?")
            who[player] = (bot, model, session, cls)
            measure_mover(study, mine, events[events["player"] == player], speeds[speeds["player"] == player], state,
                          info, bot, model, session, shard.combat, player)
        tallies += list(shard.moves.get(r.MOVE_TALLY))
        measure_tracks(study, tr.build_tracks(shard.moves, shard.combat, sessions, hour=shard.hour.label,
                                              include_companions=True), who)
        updates = shard.moves.get(r.MAP_UPDATE)
        for mapid in np.unique(updates["map"]) if len(updates) else []:
            study.ticks.setdefault(int(mapid), []).append(
                updates["diff_ms"][updates["map"] == mapid].astype(np.float64))
    # Refusals: each mover's last (cumulative) tally.
    latest: dict[int, np.void] = {}
    for row in sorted(tallies, key=lambda row: int(row["ms"])):
        latest[int(row["player"])] = row
    for player, row in latest.items():
        bot, model, session, _ = who.get(player, (int(row["kind"]) == 1, "companion", player, "?"))
        sent, kept = int(row["sent"]), int(row["kept"])
        if sent > 0:
            study.obs.add("tally sent", bot, model, session, ALL, float(sent))
            study.obs.add("tally refused", bot, model, session, ALL, float(max(0, sent - kept)))
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


# Statistics.

def quantile_distance(a: np.ndarray, b: np.ndarray, norm: str) -> float:
    """The mean |Q_b(q) - Q_a(q)| over q = 5%..95%; over the players' (`a`) median when `norm` is "median" (strictly
    positive quantities only)."""
    gap = float(np.mean(np.abs(np.quantile(b, QUANTILES) - np.quantile(a, QUANTILES))))
    if norm == "median":
        gap /= max(abs(float(np.median(a))), 1e-9)
    return gap


def _side_value(values: np.ndarray, how: str) -> float:
    if how == "max":
        return float(np.max(values))
    if how == "mean":
        return float(np.mean(values))
    if how == "p1":
        return float(np.quantile(values, 0.01))
    if how == "apex":
        launch = float(np.mean(values))
        return launch * launch / (2 * GRAVITY)
    return float(np.median(values))


def emd(players: dict, bots: dict, features: tuple[str, ...]) -> float:
    """realism.score's distance from summed histograms: per motion context both have, the mean over `features` of
    motion.histogram_distance, weighted by the bots' steps there."""
    total = weighted = 0.0
    for context, bh in bots.items():
        ph = players.get(context)
        if not ph:
            continue
        distances = [motion.histogram_distance(bh[f], ph[f], motion.HIST_BINS[f]) for f in features
                     if f in bh and f in ph]
        distances = [d for d in distances if math.isfinite(d)]
        if not distances:
            continue
        steps = float(np.sum(next(iter(bh.values()))))
        total += steps
        weighted += steps * float(np.mean(distances))
    return weighted / total if total > 0 else float("nan")


def _sum_hists(parts: list[dict]) -> dict:
    out: dict = {}
    for part in parts:
        for context, hists in part.items():
            slot = out.setdefault(context, {})
            for feature, counts in hists.items():
                slot[feature] = slot.get(feature, 0) + counts
    return out


@dataclass
class Sample:
    """One side's data for one row, per session: values, or (events, exposure) for a rate, or histograms for an EMD;
    and its n (samples, events, packets or steps)."""

    sessions: list
    n: int

    @property
    def count(self) -> int:
        return len(self.sessions)


def _pool(spec: Spec, parts: list):
    if spec.stat in ("rate", "refusals"):
        return (sum(e for e, _ in parts), sum(m for _, m in parts))
    if spec.stat == "emd":
        return _sum_hists(parts)
    return np.concatenate(parts) if parts else np.zeros(0)


def divergence(spec: Spec, players, bots) -> float:
    """The row's divergence from the two sides' pooled data."""
    scale = abs(spec.constant) if spec.constant and spec.norm == "constant" else 1.0
    if spec.stat == "median_diff":
        return abs(float(np.median(bots)) - float(np.median(players)))
    if spec.stat == "qdist":
        return quantile_distance(players, bots, spec.norm)
    if spec.stat == "rate":
        pr, br = players[0] / players[1], bots[0] / bots[1]
        return abs(br - pr) / max(pr, 1e-9)
    if spec.stat == "refusals":
        return 1000.0 * (bots[0] / bots[1] - players[0] / players[1])
    if spec.stat == "emd":
        return emd(players, bots, ("accel",) if spec.name.startswith("acceleration") else motion.HIST_FEATURES)
    if spec.stat == "parity":
        return abs(_side_value(bots, spec.how) - _side_value(players, spec.how)) / scale
    if spec.stat == "calibration":
        gap = (_side_value(players if spec.side == "players" else bots, spec.how) - (spec.constant or 0.0)) / scale
        return gap if spec.upper else abs(gap)
    raise ValueError(spec.stat)


def _shown(spec: Spec, data) -> float | None:
    """A side's value in the row."""
    if spec.stat in ("rate", "refusals"):
        return data[0] / data[1] * (1000.0 if spec.stat == "refusals" else 1.0) if data[1] else None
    if spec.stat == "emd" or not len(data):
        return None
    return _side_value(data, spec.how if spec.stat in ("parity", "calibration") else "median")


def bootstrap(spec: Spec, players: Sample, bots: Sample, resamples: int, rng: np.random.Generator
              ) -> tuple[float, float, float, float]:
    """The divergence, its CONFIDENCE interval, and the one-sided p of "at or below the threshold", from `resamples`
    resamples of each side's sessions with replacement (a side with no sessions -- the unread side of a calibration --
    stays empty)."""
    def draw(sample: Sample) -> list:
        return [sample.sessions[j] for j in rng.integers(0, sample.count, sample.count)] if sample.count else []

    point = divergence(spec, _pool(spec, players.sessions), _pool(spec, bots.sessions))
    draws = np.array([divergence(spec, _pool(spec, draw(players)), _pool(spec, draw(bots)))
                      for _ in range(resamples)])
    draws = draws[np.isfinite(draws)]
    if not len(draws):
        return point, float("nan"), float("nan"), 1.0
    alpha = (1.0 - CONFIDENCE) / 2.0
    low, high = float(np.quantile(draws, alpha)), float(np.quantile(draws, 1.0 - alpha))
    p = (float(np.sum(draws <= spec.threshold)) + 1.0) / (len(draws) + 1.0)
    return point, low, high, p


def enough(spec: Spec, sample: Sample) -> bool:
    samples, sessions = (MIN_DIST_SAMPLES, MIN_DIST_SESSIONS) if spec.distributional \
        else (MIN_STAT_SAMPLES, MIN_STAT_SESSIONS)
    return sample.n >= samples and sample.count >= sessions


def benjamini_hochberg(pvalues: list[float], fdr: float = FDR) -> list[bool]:
    """Which of `pvalues` are significant at false discovery rate `fdr`."""
    m = len(pvalues)
    order = np.argsort(pvalues)
    passed = 0
    for rank, index in enumerate(order, start=1):
        if pvalues[index] <= fdr * rank / m:
            passed = rank
    keep = {int(i) for i in order[:passed]}
    return [i in keep for i in range(m)]


# Strata.

def pooled(key: tuple, level: int) -> tuple:
    """`key` with the first `level` dimensions of POOL pooled ("*"); past them, `all`."""
    if level > len(POOL):
        return ALL
    dropped = set(POOL[:level])
    return tuple("*" if dim in dropped else value for dim, value in zip(DIMS, key))


def stratum_name(key: tuple) -> str:
    if key == ALL:
        return "all"
    return " ".join(f"{dim}={value}" for dim, value in zip(DIMS, key) if value != "*")


class Table:
    """The observations as arrays, and each side's per-session data in a stratum."""

    def __init__(self, study: Study):
        o = study.obs
        self.bot = np.asarray(o.bot, dtype=bool)
        self.model = np.asarray(o.model, dtype=object)
        self.session = np.asarray(o.session, dtype=np.int64)
        self.keys = np.asarray(o.key, dtype=object).reshape(len(o.key), len(DIMS)) if o.key \
            else np.zeros((0, len(DIMS)), dtype=object)
        self.value = np.asarray(o.value, dtype=np.float64)
        self.hists = study.hists
        metric = np.asarray(o.metric, dtype=object)
        self.by_metric = {m: np.flatnonzero(metric == m) for m in set(o.metric)}

    def select(self, metric: str, bot: bool, model: str, stratum: tuple) -> np.ndarray:
        rows = self.by_metric.get(metric, np.zeros(0, dtype=np.int64))
        rows = rows[self.bot[rows] == bot]
        if bot:
            rows = rows[self.model[rows] == model]
        for d, value in enumerate(stratum):
            if value != "*" and len(rows):
                rows = rows[self.keys[rows, d] == value]
        return rows

    def bot_keys(self, metric: str, model: str) -> list[tuple]:
        rows = self.select(metric, True, model, ALL)
        return sorted({tuple(self.keys[i]) for i in rows})

    def sample(self, spec: Spec, bot: bool, model: str, stratum: tuple) -> Sample:
        if spec.stat == "emd":
            cls = stratum[0]
            parts = [h for (b, m, _, c), h in self.hists.items() if b == bot and (m == model or not bot) and c == cls]
            return Sample(parts, int(sum(np.sum(next(iter(ctx.values()))) for part in parts for ctx in part.values())))
        if spec.stat == "refusals":
            per: dict[int, list[float]] = {}
            for i in self.select("tally refused", bot, model, ALL):
                per.setdefault(int(self.session[i]), [0.0, 0.0])[0] += self.value[i]
            for i in self.select("tally sent", bot, model, ALL):
                per.setdefault(int(self.session[i]), [0.0, 0.0])[1] += self.value[i]
            per = {s: v for s, v in per.items() if v[1] > 0}
            return Sample([tuple(v) for v in per.values()], int(sum(v[1] for v in per.values())))
        rows = self.select(spec.metric, bot, model, stratum)
        if spec.stat == "rate":
            per = {}
            for i in self.select("minutes", bot, model, stratum):
                per.setdefault(int(self.session[i]), [0.0, 0.0])[1] += self.value[i]
            for i in rows:
                per.setdefault(int(self.session[i]), [0.0, 0.0])[0] += 1.0
            per = {s: v for s, v in per.items() if v[1] > 0}
            return Sample([tuple(v) for v in per.values()], int(sum(v[0] for v in per.values())))
        groups: dict[int, list[float]] = {}
        for i in rows:
            groups.setdefault(int(self.session[i]), []).append(self.value[i])
        return Sample([np.asarray(v) for v in groups.values()], len(rows))


def _strata(spec: Spec, table: Table, model: str) -> list[tuple]:
    """`all`, and each bot stratum at the finest pooling where the row's sides meet the minimum."""
    out = [ALL]
    if spec.stat == "refusals":
        return out
    if spec.stat == "emd":
        return out + [(c,) + ALL[1:] for c in sorted({c for (b, m, _, c) in table.hists if b and m == model and
                                                      c != "*"})]
    for key in table.bot_keys(spec.metric, model):
        for level in range(len(POOL) + 2):
            stratum = pooled(key, level)
            if stratum == ALL:
                break
            players, bots = table.sample(spec, False, model, stratum), table.sample(spec, True, model, stratum)
            measured_ok = enough(spec, players if spec.side == "players" else bots) if spec.stat == "calibration" \
                else enough(spec, players) and enough(spec, bots)
            if measured_ok:
                if stratum not in out:
                    out.append(stratum)
                break
    return out


def _row(spec: Spec, table: Table, model: str, stratum: tuple, resamples: int, rng: np.random.Generator) -> dict:
    players = table.sample(spec, False, model, stratum)
    bots = table.sample(spec, True, model, stratum)
    out = {"section": spec.section, "kind": spec.kind, "model": model, "context": stratum_name(stratum),
           "metric": spec.name, "threshold": spec.threshold, "norm": spec.norm,
           "n": {"players": [players.n, players.count], "bots": [bots.n, bots.count]},
           "players": _shown(spec, _pool(spec, players.sessions)) if players.count else None,
           "bots": _shown(spec, _pool(spec, bots.sessions)) if bots.count else None,
           "divergence": None, "interval": None, "p": None}
    if spec.unit:
        out["unit"] = spec.unit
    if spec.stat in ("parity", "calibration"):
        out["constant"] = spec.constant
    if spec.stat == "calibration":
        measured = players if spec.side == "players" else bots
        if not enough(spec, measured):
            out["flag"] = f"inconclusive (n {measured.n} in {measured.count} sessions)"
            return out
        empty = Sample([], 0)
        result = bootstrap(spec, players if spec.side == "players" else empty,
                           bots if spec.side == "bots" else empty, resamples, rng)
    else:
        if not (enough(spec, players) and enough(spec, bots)):
            out["flag"] = (f"inconclusive (n players {players.n} in {players.count} sessions, bots {bots.n} in "
                           f"{bots.count})")
            return out
        result = bootstrap(spec, players, bots, resamples, rng)
    point, low, high, p = result
    out.update(divergence=point, interval=[low, high], p=p)
    if not (math.isfinite(low) and math.isfinite(high)):
        out["flag"] = "inconclusive (no interval)"
    elif low > spec.threshold:
        out["flag"] = "attention"
    elif high <= spec.threshold:
        out["flag"] = "pass"
    else:
        out["flag"] = "inconclusive"
    return out


def compare(study: Study, resamples: int = BOOTSTRAP, seed: int = 0) -> list[dict]:
    table = Table(study)
    rng = np.random.default_rng(seed)
    models = sorted({m for m, b in zip(study.obs.model, study.obs.bot) if b}
                    | {m for (b, m, _, _) in study.hists if b})
    rows: list[dict] = []
    for model in models:
        for spec in SPECS:
            for stratum in _strata(spec, table, model):
                rows.append(_row(spec, table, model, stratum, resamples, rng))
        for name in REPORTED:
            for bot in (False, True):
                values = table.value[table.select(name, bot, model, ALL)]
                rows.append({"section": "reported", "kind": "controller", "model": model, "context": "all",
                             "metric": name, "side": "bots" if bot else "players",
                             "median": float(np.median(values)) if len(values) else None, "count": int(len(values)),
                             "flag": "reported"})
    for section in SECTIONS:
        tested = [row for row in rows if row["section"] == section and row.get("p") is not None]
        for row, significant in zip(tested, benjamini_hochberg([row["p"] for row in tested]) if tested else []):
            row["survives"] = bool(significant and row["flag"] == "attention")
    return rows


# Realm timing.

def _quantiles(values: np.ndarray) -> dict:
    if not len(values):
        return {"n": 0}
    return {"n": int(len(values)), "p5": float(np.quantile(values, 0.05)), "p50": float(np.median(values)),
            "p95": float(np.quantile(values, 0.95)), "max": float(np.max(values)), "mean": float(np.mean(values))}


def timing(study: Study) -> dict:
    """The realm's tick and decision intervals, and whether they call for training jitter."""
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


# The report.

def proposals(rows: list[dict]) -> list[dict]:
    """Constant changes the players' own measurements propose (for the user): a players-vs-constant row in `all` with
    its interval wholly past the threshold, surviving correction, on CONSTANT_CHANGE_N samples or more."""
    return [{"constant": row["metric"].split(" (players")[0], "value": row.get("constant"),
             "players_measured": row["players"], "divergence": row["divergence"], "interval": row["interval"],
             "n": row["n"]["players"]}
            for row in rows
            if row["section"] == "physics" and row["metric"].endswith("(players vs constant)")
            and row["context"] == "all" and row.get("survives") and row["n"]["players"][0] >= CONSTANT_CHANGE_N]


def report(study: Study, source: str = "", resamples: int = BOOTSTRAP, seed: int = 0) -> dict:
    rows = compare(study, resamples, seed)
    sections = {}
    for section in SECTIONS:
        mine = [row for row in rows if row["section"] == section]
        tested = [row for row in mine if row.get("p") is not None]
        sections[section] = {
            "rows": len(mine), "tested": len(tested),
            "attention": sum(1 for row in mine if row["flag"] == "attention"),
            "expected_by_chance": round((1.0 - CONFIDENCE) / 2.0 * len(tested), 2),
            "survive_correction": sum(1 for row in mine if row.get("survives")),
            "pass": sum(1 for row in mine if row["flag"] == "pass"),
            "inconclusive": sum(1 for row in mine if str(row["flag"]).startswith("inconclusive")),
        }
    headline = [row for row in rows if row["section"] != "reported"
                and (row["context"] == "all" or row.get("survives"))]
    return {"format": 2, "built": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"), "source": source,
            "clock": study.clocks,
            "bootstrap": {"resamples": resamples, "confidence": CONFIDENCE, "by": "session", "fdr": FDR},
            "minimums": {"distribution": [MIN_DIST_SAMPLES, MIN_DIST_SESSIONS],
                         "rate_or_median": [MIN_STAT_SAMPLES, MIN_STAT_SESSIONS]},
            "kinds": {"controller": "tests the controller and the client's rules",
                      "behaviour": "the policy's choices: informs training (style, the human-likeness reward); "
                                   "never gates a controller change"},
            "not_measured": {"FLOAT_DEPTH": "needs the liquid's level, which no capture record holds"},
            "synthesised_companion_samples": study.synthesised,
            "sections": sections, "proposals": proposals(rows), "timing": timing(study),
            "attention": [row for row in rows if row.get("survives")],
            "headline": headline, "rows": rows}


def _fmt(value) -> str:
    if value is None:
        return "-"
    if isinstance(value, float):
        return "nan" if not math.isfinite(value) else f"{value:.4g}"
    if isinstance(value, list) and len(value) == 2:
        return f"[{_fmt(value[0])}, {_fmt(value[1])}]"
    return str(value)


def markdown(rep: dict) -> str:
    lines = [f"# Bot / player parity ({rep['source']})", "",
             f"Clock: {rep['clock']}. Bootstrap {rep['bootstrap']['resamples']} resamples by session, 95% intervals; "
             f"Benjamini-Hochberg at q {FDR} per section. `attention`: the interval wholly above the threshold; "
             "`pass`: wholly at or below. Controller rows test the controller; behaviour rows inform training and "
             "never gate a controller change. Nothing here decides: the user does.", "",
             "| section | rows | tested | attention | expected by chance | survive correction | pass | inconclusive |",
             "|---|---|---|---|---|---|---|---|"]
    for name, s in rep["sections"].items():
        lines.append(f"| {name} | {s['rows']} | {s['tested']} | {s['attention']} | {s['expected_by_chance']} | "
                     f"{s['survive_correction']} | {s['pass']} | {s['inconclusive']} |")
    lines.append("")
    if rep["proposals"]:
        lines += ["## Constant changes the players' measurements propose (for the user)", ""]
        lines += [f"- {p['constant']}: constant {_fmt(p['value'])}, players {_fmt(p['players_measured'])} "
                  f"(divergence {_fmt(p['divergence'])}, interval {_fmt(p['interval'])}, n {p['n']})"
                  for p in rep["proposals"]]
        lines.append("")

    def table(rows: list[dict]) -> list[str]:
        out = ["| flag | survives | kind | model | context | metric | players | bots | constant | divergence | "
               "95% interval | threshold | norm | n players (sessions) | n bots (sessions) |",
               "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for row in rows:
            n = row["n"]
            out.append(f"| {row['flag']} | {'yes' if row.get('survives') else ''} | {row['kind']} | {row['model']} | "
                       f"{row['context']} | {row['metric']} | {_fmt(row['players'])} | {_fmt(row['bots'])} | "
                       f"{_fmt(row.get('constant'))} | {_fmt(row['divergence'])} | {_fmt(row['interval'])} | "
                       f"{_fmt(row['threshold'])} | {row['norm']} | {n['players'][0]} ({n['players'][1]}) | "
                       f"{n['bots'][0]} ({n['bots'][1]}) |")
        return out

    lines += ["## Headline (`all`, and the attentions surviving correction)", ""] + table(rep["headline"]) + [""]
    for section in SECTIONS:
        rows = [row for row in rep["rows"] if row["section"] == section and row["context"] != "all"]
        if rows:
            lines += [f"## {section} by stratum", ""] + table(rows) + [""]
    reported = [row for row in rep["rows"] if row["section"] == "reported"]
    if reported:
        lines += ["## Reported (no threshold)", ""]
        lines += [f"- {row['metric']}, {row['side']} ({row['model']}): median {_fmt(row['median'])}, "
                  f"n {row['count']}" for row in reported]
        lines.append("")
    t = rep["timing"]

    def q(d: dict) -> str:
        if not d.get("n"):
            return "not in the capture"
        return f"p5 {d['p5']:.1f}, p50 {d['p50']:.1f}, p95 {d['p95']:.1f}, max {d['max']:.1f} ms (n {d['n']})"

    verdict = {None: "not measured", True: "recommended (" + "; ".join(t["reasons"]) + ")",
               False: "not needed"}[t["jitter_recommended"]]
    lines += ["## Realm timing", "", f"- world tick (MapUpdate): {q(t['tick_ms'])}",
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
