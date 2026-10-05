"""The executor-fit study (plan §2.3): how much of human motion can the bot's movement actions express?

A Python kinematic emulator of MoveBlock's semantics (src/server/game/Animus/Scenario/Curriculum/Blocks/
MoveBlock.h/.cpp), and a beam search per human clip for the action sequence that best reproduces it at the decision
rate. Per context it reports how far the best reproduction drifts and how its motion's distributions differ from
the human's, for the current action space and the alternatives a change might adopt.

**The emulator** (one action a decision, as the policy has):
- Bearings are egocentric and durative: BEARING b walks heading `facing - b * 45 deg` until another bearing or HALT,
  at the run speed in force, or the back speed (run_back / run, swim_back / swim, flight_back / flight from the
  core's base speeds) for the three bearings behind. A held bearing is assumed refreshed for free (re-pressing is
  uncharged and keeps it from lapsing after Options.MoveBearingMs).
- TURN_ANGLES (+-15, +-45, +-90, +-135, 180 deg; left positive) set what is left of a turn, carried out at TURN_RATE
  (45 deg) a decision; exactly one turn step a decision (MoveBlock's TurnStepped): a turn under way steps before the
  action, a turn chosen on a decision with none under way steps at once. Revision 1 adds FINE_TURN (+-5 deg).
- PITCH_ANGLES (-60..60 every 15 deg) set the pitch target, reached at PITCH_RATE (30 deg) a decision, the same way;
  only swimming or flying (masked on the ground), and the target already chosen is masked.
- FACE_HEADING snaps the facing onto the held bearing's heading and makes the bearing forward, every decision it is
  the mode; FACE_HOLD keeps the facing; the mode already held is masked. FACE_TARGET needs the target's position,
  which the fit does not have: it is left out (an under-statement for fights, where it is the strafe-circle).
- JUMP (ground only, not mid-jump) clears the bearing and carries the body JUMP_SECONDS (2 * 7.955 / 19.29) along
  the facing at the run speed, bearings masked meanwhile.
- Within a decision the body moves along the heading half-way through that decision's turn step: the chord of the
  arc a TurnRun spline walks.

**Limits.** Terrain and collision are ignored (human tracks are already feasible, so this measures the action space
and not the pathfinder); on the ground only planar error counts (the ground owns z), swimming and flying are 3D.
Splines' acceleration, MoveKeep's relaunch rules and corner smoothing are not modelled. The human's mode, speed
and mount are given to the emulator per decision, not chosen.

**Search.** Per chunk of CHUNK_SECONDS, re-anchored at the human's position, facing and pitch with each of the
nine bearing states (none or one of eight) as a start, a beam of `beam` states expands every action each decision
and keeps the cheapest by cumulative cost: squared position error (yards) + (YAW_WEIGHT * heading error in
radians)^2 + PRESS_COST per press (the tie-break that prefers one 90-degree turn to two 45s, and no press to a
re-press). **Drift at 1/2/5 s** is the best path's position error that long after the chunk's anchor; **heading
error** its mean absolute facing error. **Distribution distances** are motion.histogram_distance between the
emulated and the human steps' motion.features per context: yaw_rate (curvature), planar and accel (the speed
profile), fwd and lat. A chunk is **expressible** when its 2 s drift is within EXPRESSIBLE_YARDS and its heading
error within EXPRESSIBLE_DEGREES.

**Action spaces** evaluated (`SPACES`): `lattice` (revision 0), `lattice_fine` (+ the revision 1 fine turns),
`turn_rate` (the turns replaced by a held turn rate of -180, -60, 0, 60 or 180 deg/s, which also makes strafe+turn
arcs a single held state), and `lattice_125ms` (the lattice at a 125 ms decision, its turn and pitch steps halved
so the rates per second are the game's).
"""

from __future__ import annotations

import datetime as dt
import json
import math
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from animus.human import motion

# MoveBlock.h
TURN_ANGLES = (0.2617994, -0.2617994, 0.7853982, -0.7853982, 1.5707963, -1.5707963, 2.3561945, -2.3561945,
               3.1415927)
PITCH_ANGLES = (-1.0471976, -0.7853982, -0.5235988, -0.2617994, 0.0, 0.2617994, 0.5235988, 0.7853982, 1.0471976)
TURN_RATE = 0.7853982           # per 250 ms decision
PITCH_RATE = 0.5235988
FINE_TURN = 0.0872665
JUMP_SPEED_Z = 7.955
GRAVITY = 19.29111              # Movement::gravity
JUMP_SECONDS = 2.0 * JUMP_SPEED_Z / GRAVITY
BEARINGS = 8
BACK_BEARINGS = (3, 4, 5)
# Back speed over forward speed per mode (Unit.cpp baseMoveSpeed): run_back/run, swim_back/swim, flight_back/flight.
BACK_RATIO = {motion.MODE_GROUND: 4.5 / 7.0, motion.MODE_SWIM: 2.5 / 4.722222, motion.MODE_FLY: 4.5 / 7.0,
              motion.MODE_AIRBORNE: 4.5 / 7.0}
TURN_RATES = (-math.pi, -math.pi / 3.0, 0.0, math.pi / 3.0, math.pi)   # rad/s, the turn_rate space

CHUNK_SECONDS = 5.0
YAW_WEIGHT = 2.0                # yards per radian of heading error in the cost
PRESS_COST = 1e-4
EXPRESSIBLE_YARDS = 1.0
EXPRESSIBLE_DEGREES = 10.0
DIST_FEATURES = ("yaw_rate", "planar", "accel", "fwd", "lat")

# Action kinds.
NOOP, BEARING, HALT, FACE_HEADING, FACE_HOLD, TURN, PITCH, JUMP, RATE = range(9)
KIND_NAMES = ("noop", "bearing", "halt", "face_heading", "face_hold", "turn", "pitch", "jump", "rate")
FACE_MODE_HOLD, FACE_MODE_HEADING = 0, 1


@dataclass(frozen=True)
class Space:
    """An action space: its actions as (kind, value) and its decision interval."""

    name: str
    actions: tuple[tuple[int, float], ...]
    dt: float = motion.DECISION_SECONDS
    turn_step: float = TURN_RATE
    pitch_step: float = PITCH_RATE

    def label(self, index: int) -> str:
        kind, value = self.actions[index]
        if kind == BEARING:
            return f"bearing_{int(value)}"
        if kind in (TURN, PITCH):
            return f"{KIND_NAMES[kind]}_{round(math.degrees(value)):+d}"
        if kind == RATE:
            return f"rate_{round(math.degrees(value)):+d}"
        return KIND_NAMES[kind]


def _base(turns: tuple[float, ...] = TURN_ANGLES, rates: tuple[float, ...] = ()) -> tuple:
    acts = [(NOOP, 0.0)] + [(BEARING, float(b)) for b in range(BEARINGS)] + [(HALT, 0.0), (FACE_HEADING, 0.0),
                                                                             (FACE_HOLD, 0.0)]
    acts += [(TURN, a) for a in turns] + [(RATE, r) for r in rates]
    acts += [(PITCH, p) for p in PITCH_ANGLES] + [(JUMP, 0.0)]
    return tuple(acts)


SPACES = {
    "lattice": Space("lattice", _base()),
    "lattice_fine": Space("lattice_fine", _base(TURN_ANGLES + (FINE_TURN, -FINE_TURN))),
    "turn_rate": Space("turn_rate", _base(turns=(), rates=TURN_RATES)),
    "lattice_125ms": Space("lattice_125ms", _base(), dt=0.125, turn_step=TURN_RATE / 2, pitch_step=PITCH_RATE / 2),
}


@dataclass
class State:
    """A batch of emulator states (every field an array of the batch's length)."""

    x: np.ndarray
    y: np.ndarray
    z: np.ndarray
    facing: np.ndarray
    pitch: np.ndarray
    pitch_target: np.ndarray
    turn_left: np.ndarray
    bearing: np.ndarray          # -1: none held
    face_mode: np.ndarray
    jump_left: np.ndarray
    jump_heading: np.ndarray
    rate: np.ndarray

    @staticmethod
    def start(n: int, x: float, y: float, z: float, facing: float, pitch: float = 0.0,
              bearing: np.ndarray | int = -1) -> "State":
        f = lambda v: np.full(n, v, dtype=np.float64)  # noqa: E731
        return State(f(x), f(y), f(z), f(facing), f(pitch), f(pitch), f(0.0),
                     np.broadcast_to(np.asarray(bearing, dtype=np.int64), (n,)).copy(),
                     np.zeros(n, dtype=np.int64), f(0.0), f(0.0), f(0.0))

    def take(self, index: np.ndarray) -> "State":
        return State(*(getattr(self, name)[index] for name in self.__dataclass_fields__))


def step(state: State, kinds: np.ndarray, values: np.ndarray, mode: int, speed: float, space: Space
         ) -> tuple[State, np.ndarray]:
    """One decision for a batch: the action (kind, value) of each state, then the decision's motion. Returns the
    new states and which actions were allowed (MoveBlock's mask)."""
    s = State(*(getattr(state, name).copy() for name in state.__dataclass_fields__))
    n = len(kinds)
    airborne = mode in (motion.MODE_SWIM, motion.MODE_FLY)
    jumping = s.jump_left > 1e-9
    held = s.bearing >= 0
    allowed = np.ones(n, dtype=bool)
    allowed[(kinds == BEARING) & jumping] = False
    allowed[(kinds == HALT) & (~held | jumping)] = False
    allowed[(kinds == FACE_HEADING) & (s.face_mode == FACE_MODE_HEADING)] = False
    allowed[(kinds == FACE_HOLD) & (s.face_mode == FACE_MODE_HOLD)] = False
    allowed[(kinds == PITCH) & (not airborne)] = False
    allowed[(kinds == PITCH) & (np.abs(values - s.pitch_target) <= 1e-3)] = False
    allowed[(kinds == JUMP) & (airborne or mode == motion.MODE_AIRBORNE)] = False
    allowed[(kinds == JUMP) & jumping] = False

    before = s.facing.copy()
    # A held turn rate (the turn_rate space) is chosen first and turns the whole decision.
    is_rate = kinds == RATE
    s.rate[is_rate] = values[is_rate]
    s.facing += s.rate * space.dt
    # A turn under way steps before the action; a new one replaces what is left and steps now only if none did.
    stepped = np.abs(s.turn_left) > 1e-6
    turn = np.clip(s.turn_left, -space.turn_step, space.turn_step) * stepped
    s.facing += turn
    s.turn_left -= turn
    is_turn = kinds == TURN
    s.turn_left[is_turn] = values[is_turn]
    fresh = is_turn & ~stepped
    turn = np.clip(s.turn_left, -space.turn_step, space.turn_step) * fresh
    s.facing += turn
    s.turn_left -= turn
    s.turn_left[np.abs(s.turn_left) < 1e-6] = 0.0
    # Pitch the same way, off the ground only.
    if airborne:
        pstepped = np.abs(s.pitch_target - s.pitch) > 1e-6
        dp = np.clip(s.pitch_target - s.pitch, -space.pitch_step, space.pitch_step) * pstepped
        s.pitch += dp
        is_pitch = kinds == PITCH
        s.pitch_target[is_pitch] = values[is_pitch]
        pfresh = is_pitch & ~pstepped
        s.pitch += np.clip(s.pitch_target - s.pitch, -space.pitch_step, space.pitch_step) * pfresh
    else:
        s.pitch[:] = 0.0
        s.pitch_target[:] = 0.0
    # Feet and facing modes.
    is_bearing = (kinds == BEARING) & allowed
    s.bearing[is_bearing] = values[is_bearing].astype(np.int64)
    s.bearing[(kinds == HALT) & allowed] = -1
    s.face_mode[kinds == FACE_HEADING] = FACE_MODE_HEADING
    s.face_mode[kinds == FACE_HOLD] = FACE_MODE_HOLD
    launch = (kinds == JUMP) & allowed
    s.bearing[launch] = -1
    s.jump_left[launch] = JUMP_SECONDS
    s.jump_heading[launch] = s.facing[launch]
    snap = (s.face_mode == FACE_MODE_HEADING) & (s.bearing > 0)
    s.facing[snap] = s.facing[snap] - s.bearing[snap] * (2 * math.pi / BEARINGS)
    s.bearing[snap] = 0
    # Moving: along the heading half-way through the decision's turn (a snap is a snap, not a turn).
    swing = motion.wrap(s.facing - before)
    mid = np.where(snap, s.facing, before + swing / 2.0)
    walking = (s.bearing >= 0) & (s.jump_left <= 1e-9)
    heading = mid - np.maximum(s.bearing, 0) * (2 * math.pi / BEARINGS)
    back = np.isin(s.bearing, BACK_BEARINGS)
    v = speed * np.where(back, BACK_RATIO.get(mode, 1.0), 1.0) * walking
    dist = v * space.dt
    if airborne:
        pitch = np.where(back, -s.pitch, s.pitch)
        s.x += dist * np.cos(pitch) * np.cos(heading)
        s.y += dist * np.cos(pitch) * np.sin(heading)
        s.z += dist * np.sin(pitch)
    else:
        s.x += dist * np.cos(heading)
        s.y += dist * np.sin(heading)
    flying = s.jump_left > 1e-9
    hop = np.minimum(s.jump_left, space.dt) * speed * flying
    s.x += hop * np.cos(s.jump_heading)
    s.y += hop * np.sin(s.jump_heading)
    s.jump_left = np.maximum(0.0, s.jump_left - space.dt)
    s.facing = np.remainder(s.facing, 2 * math.pi)
    return s, allowed


def rollout(space: Space, actions: list[int], start: dict, modes: np.ndarray, speeds: np.ndarray) -> np.ndarray:
    """Emulate one action sequence from `start` (x, y, z, facing, pitch, bearing): [len(actions) + 1, 5] rows of
    x, y, z, facing, pitch."""
    s = State.start(1, start["x"], start["y"], start.get("z", 0.0), start["facing"], start.get("pitch", 0.0),
                    start.get("bearing", -1))
    out = [[s.x[0], s.y[0], s.z[0], s.facing[0], s.pitch[0]]]
    for k, a in enumerate(actions):
        kind, value = space.actions[a]
        speed = float(speeds[k]) if speeds[k] > 0.1 else motion.DEFAULT_SPEED
        s, _ = step(s, np.array([kind]), np.array([value]), int(modes[k]), speed, space)
        out.append([s.x[0], s.y[0], s.z[0], s.facing[0], s.pitch[0]])
    return np.asarray(out)


@dataclass
class FitResult:
    actions: list[int]
    start_bearing: int
    path: np.ndarray                # [K + 1, 5] x, y, z, facing, pitch
    pos_err: np.ndarray             # [K + 1]
    yaw_err: np.ndarray             # [K + 1] radians, absolute
    cost: float


def beam_fit(space: Space, human: np.ndarray, beam: int = 32, start_bearings: tuple[int, ...] | None = None,
             jumps: np.ndarray | None = None) -> FitResult:
    """The action sequence of `space` that best reproduces `human` ([K + 1, SAMPLE_DIM] on space.dt), started at
    the human's first sample with any bearing held (or none), or only `start_bearings` when the start is known.
    `jumps` ([K] bool, from the MSG_MOVE_JUMP packets) pins the jumps: JUMP where the human jumped (when a jump can
    be taken there) and nowhere else -- on the ground a jump's carry is otherwise a forward walk's."""
    h = np.asarray(human, dtype=np.float64)
    k_steps = len(h) - 1
    kinds = np.array([a[0] for a in space.actions])
    values = np.array([a[1] for a in space.actions], dtype=np.float64)
    n_act = len(kinds)
    starts = np.asarray(start_bearings if start_bearings is not None else range(-1, BEARINGS), dtype=np.int64)
    state = State.start(len(starts), h[0, motion.X], h[0, motion.Y], h[0, motion.Z], h[0, motion.YAW],
                        h[0, motion.PITCH], starts)
    total = np.zeros(len(starts))
    origin = np.arange(len(starts))         # which start each beam entry came from
    history: list[tuple[np.ndarray, np.ndarray]] = []   # per step: parent index, action
    for k in range(k_steps):
        m = len(total)
        rep = np.repeat(np.arange(m), n_act)
        acts = np.tile(np.arange(n_act), m)
        cand, allowed = step(state.take(rep), kinds[acts], values[acts], int(h[k, motion.MODE]),
                             float(h[k, motion.SPEED]) if h[k, motion.SPEED] > 0.1 else motion.DEFAULT_SPEED, space)
        target = h[k + 1]
        if int(target[motion.MODE]) in (motion.MODE_SWIM, motion.MODE_FLY):
            err2 = ((cand.x - target[motion.X]) ** 2 + (cand.y - target[motion.Y]) ** 2
                    + (cand.z - target[motion.Z]) ** 2)
        else:
            cand.z[:] = target[motion.Z]
            err2 = (cand.x - target[motion.X]) ** 2 + (cand.y - target[motion.Y]) ** 2
        yaw = motion.wrap(cand.facing - target[motion.YAW])
        cost = total[rep] + err2 + (YAW_WEIGHT * yaw) ** 2 + PRESS_COST * (kinds[acts] != NOOP)
        cost[~allowed] = np.inf
        if jumps is not None:
            is_jump = kinds[acts] == JUMP
            if jumps[k] and (allowed & is_jump).any():
                cost[~is_jump] = np.inf
            else:
                cost[is_jump] = np.inf
        keep = np.argsort(cost, kind="stable")[:beam]
        keep = keep[np.isfinite(cost[keep])]
        state = cand.take(keep)
        total = cost[keep]
        history.append((rep[keep], acts[keep]))
        origin = origin[rep[keep]]
    best = int(np.argmin(total)) if len(total) else 0
    actions = []
    idx = best
    for parents, acts in reversed(history):
        actions.append(int(acts[idx]))
        idx = int(parents[idx])
    actions.reverse()
    start_bearing = int(starts[idx])
    path = rollout(space, actions, {"x": h[0, motion.X], "y": h[0, motion.Y], "z": h[0, motion.Z],
                                    "facing": h[0, motion.YAW], "pitch": h[0, motion.PITCH],
                                    "bearing": start_bearing}, h[:, motion.MODE], h[:, motion.SPEED])
    ground = ~np.isin(h[:, motion.MODE].astype(int), (motion.MODE_SWIM, motion.MODE_FLY))
    path[ground, 2] = h[ground, motion.Z]
    pos_err = np.linalg.norm(path[:, :3] - h[:, motion.X:motion.Z + 1], axis=1)
    yaw_err = np.abs(motion.wrap(path[:, 3] - h[:, motion.YAW]))
    return FitResult(actions, start_bearing, path, pos_err, yaw_err, float(total[best]) if len(total) else math.inf)


def emulated_samples(human: np.ndarray, fit: FitResult) -> np.ndarray:
    """The fit's path as kinematic samples (mode, mount, speed and combat the human's) for motion.features."""
    out = np.asarray(human, dtype=np.float64).copy()
    out[:, motion.X:motion.Z + 1] = fit.path[:, :3]
    out[:, motion.YAW] = fit.path[:, 3]
    out[:, motion.PITCH] = fit.path[:, 4]
    return out


def chunks(samples: np.ndarray, dt: float, seconds: float = CHUNK_SECONDS) -> list[np.ndarray]:
    """Non-overlapping chunks of `seconds` (plus the anchor sample) of a clip on the `dt` grid."""
    n = int(round(seconds / dt))
    return [samples[i:i + n + 1] for i in range(0, len(samples) - n, n)]


@dataclass
class FitStudy:
    """Fits accumulated per space and context; `report` summarises them."""

    beam: int = 32
    spaces: tuple[str, ...] = tuple(SPACES)
    rows: dict[str, dict[int, list[dict]]] = field(default_factory=dict)
    feats: dict[str, dict[int, list[tuple[np.ndarray, np.ndarray]]]] = field(default_factory=dict)

    def add_clip(self, fine: np.ndarray) -> None:
        """One clip on the 125 ms grid (the 250 ms spaces read every other sample)."""
        for name in self.spaces:
            space = SPACES[name]
            every = int(round(space.dt / 0.125))
            grid = fine[::every]
            for chunk in chunks(grid, space.dt):
                fit = beam_fit(space, chunk, self.beam)
                ctx = int(np.bincount(motion.step_contexts(chunk), minlength=motion.CONTEXTS).argmax())
                at = {s: int(round(s / space.dt)) for s in (1, 2, 5)}
                row = {f"drift_{s}s": float(fit.pos_err[i]) for s, i in at.items() if i < len(fit.pos_err)}
                row["heading_err_deg"] = float(np.degrees(fit.yaw_err[1:].mean()))
                row["presses"] = sum(1 for a in fit.actions if space.actions[a][0] != NOOP)
                self.rows.setdefault(name, {}).setdefault(ctx, []).append(row)
                # Features on the 250 ms grid whatever the space, so every space is compared on the same steps.
                dec = max(1, int(round(motion.DECISION_SECONDS / space.dt)))
                human_f = motion.features(chunk[::dec])
                bot_f = motion.features(emulated_samples(chunk, fit)[::dec])
                self.feats.setdefault(name, {}).setdefault(ctx, []).append((human_f, bot_f))

    def report(self) -> dict:
        out = {"format": 1, "built": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
               "definitions": {"chunk_seconds": CHUNK_SECONDS, "beam": self.beam, "yaw_weight": YAW_WEIGHT,
                               "press_cost": PRESS_COST, "expressible_yards": EXPRESSIBLE_YARDS,
                               "expressible_degrees": EXPRESSIBLE_DEGREES,
                               "drift": "position error of the best reproduction 1/2/5 s after the chunk's anchor",
                               "emd": "motion.histogram_distance, emulated vs human steps, per motion feature"},
               "limits": ["terrain and collision ignored: human tracks are already feasible",
                          "ground error is planar (the ground owns z); swim/fly error is 3D",
                          "FACE_TARGET left out (no target positions)",
                          "a held bearing is assumed refreshed for free (no MoveBearingMs lapse)",
                          "within a decision the body moves on the mid-turn heading (a TurnRun chord)",
                          "spline acceleration, MoveKeep relaunches and corner smoothing not modelled"],
               "spaces": {}}
        for name, per_ctx in self.rows.items():
            space_out = {"dt": SPACES[name].dt, "actions": [SPACES[name].label(i)
                                                            for i in range(len(SPACES[name].actions))],
                         "contexts": {}}
            all_rows = []
            for ctx, rows in sorted(per_ctx.items()):
                all_rows += rows
                entry = _summary(rows)
                entry["name"] = motion.context_name(ctx)
                pairs = self.feats[name][ctx]
                human_f = np.concatenate([p[0] for p in pairs])
                bot_f = np.concatenate([p[1] for p in pairs])
                entry["emd"] = {}
                for feat in DIST_FEATURES:
                    i = motion.INDEX[feat]
                    bins = motion.HIST_BINS.get(feat, np.linspace(-motion.CLIP, motion.CLIP, 61))
                    d = motion.histogram_distance(np.histogram(bot_f[:, i], bins)[0], np.histogram(human_f[:, i],
                                                                                                    bins)[0], bins)
                    entry["emd"][feat] = None if math.isnan(d) else round(d, 4)
                space_out["contexts"][str(ctx)] = entry
            space_out["overall"] = _summary(all_rows)
            out["spaces"][name] = space_out
        return out


def _summary(rows: list[dict]) -> dict:
    out: dict = {"chunks": len(rows)}
    for key in ("drift_1s", "drift_2s", "drift_5s", "heading_err_deg", "presses"):
        vals = np.asarray([r[key] for r in rows if key in r], dtype=np.float64)
        if len(vals):
            p50, p90 = np.percentile(vals, [50, 90])
            out[key] = {"p50": round(float(p50), 3), "p90": round(float(p90), 3), "mean": round(float(vals.mean()), 3)}
    ok = [r for r in rows if "drift_2s" in r]
    if ok:
        out["expressible_share"] = round(sum(1 for r in ok if r["drift_2s"] <= EXPRESSIBLE_YARDS
                                             and r["heading_err_deg"] <= EXPRESSIBLE_DEGREES) / len(ok), 4)
    return out


def markdown(report: dict) -> str:
    lines = ["# Executor fit: how much human motion the bot's move actions express", "",
             f"Built {report['built']}. Chunks of {report['definitions']['chunk_seconds']} s, re-anchored on the "
             f"human; drift is the best reproduction's position error 1/2/5 s in (yards), heading error its mean "
             f"absolute facing error, expressible the share of chunks within {EXPRESSIBLE_YARDS} yd at 2 s and "
             f"{EXPRESSIBLE_DEGREES} deg. EMD columns: emulated vs human motion-feature distributions.", "",
             "Limits: " + "; ".join(report["limits"]) + ".", ""]
    head = "| space | context | chunks | drift 1s p50/p90 | drift 2s p50/p90 | drift 5s p50/p90 | heading p50 | " \
           "expressible | EMD yaw_rate | EMD planar | EMD accel |"
    lines += [head, "|" + "---|" * (head.count("|") - 1)]
    for name, space in report["spaces"].items():
        rows = list(space["contexts"].items()) + [("all", dict(space["overall"], name="all", emd={}))]
        for _, entry in rows:
            def pp(key):
                v = entry.get(key)
                return f"{v['p50']:.2f} / {v['p90']:.2f}" if v else "-"
            emd = entry.get("emd", {})
            lines.append(f"| {name} | {entry['name']} | {entry['chunks']} | {pp('drift_1s')} | {pp('drift_2s')} | "
                         f"{pp('drift_5s')} | {entry.get('heading_err_deg', {}).get('p50', '-')} | "
                         f"{entry.get('expressible_share', '-')} | {emd.get('yaw_rate', '-')} | "
                         f"{emd.get('planar', '-')} | {emd.get('accel', '-')} |")
    return "\n".join(lines) + "\n"


def write(report: dict, out_dir: str | Path) -> tuple[Path, Path]:
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    js = out_dir / "human_fit.json"
    md = out_dir / "human_fit.md"
    js.write_text(json.dumps(report, indent=1))
    md.write_text(markdown(report))
    return js, md
