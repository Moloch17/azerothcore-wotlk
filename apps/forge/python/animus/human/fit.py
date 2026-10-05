"""The executor-fit study (plan §2.3): how much of human motion can the bot's movement actions express?

A Python kinematic emulator of the move block's action space -- a player's keys and mouse driving the player
controller (src/server/game/Animus/Scenario/Curriculum/Blocks/MoveControls.h, Animus/Movement/PlayerController.h) --
and a beam search per human clip for the action sequence that best reproduces it at the decision rate. Per context it
reports how far the best reproduction drifts and how its motion's distributions differ from the human's.

**The emulator** (one action a decision, as the policy has; MoveControls.h's 25 actions):
- Held controls persist until changed: forward / back / stop, strafe left / right / stop, a turn rate (+-360, 180, 90,
  30 deg/s or 0, left positive -- the mouse's, not slowed while moving), a pitch rate (+-90, 30 or 0 deg/s, water and
  air only), ascend / descend / stop (water and air), walk (a toggle). Pressing the value already held does nothing.
- The speed in force (the sample's) is the run, swim or flight speed; back is that times the back ratio (run_back /
  run, ...); walking caps everything at the walk speed; forward or back held with a strafe is a diagonal, each
  component x 0.7071, at the back speed when back is held (the client's rules, fn 0x987570).
- Swimming or flying, the forward direction is the look direction (pitch); a held ascend or descend moves at 45
  degrees (0.7071 of the speed each way, fn 0x987700).
- JUMP (ground or water, not mid-jump) freezes the horizontal velocity for JUMP_SECONDS (2 x 7.9555 / 19.2911): no
  air control, the turn rate still turns the body.
- Within a decision the body moves along the heading half-way through the decision's turn: the chord of the arc a
  steady turn rate walks.

**Limits.** Terrain and collision are ignored (human tracks are already feasible, so this measures the action space,
not the world); on the ground only planar error counts (the ground owns z), swimming and flying are 3D. The human's
mode, speed and mount are given per decision, not chosen. A human turns the mouse at any rate; the policy has nine,
held a decision at least -- what the fit measures.

**Search.** Per chunk of CHUNK_SECONDS, re-anchored at the human's position, facing and pitch with each of the nine
start states of the feet (forward/back/none x strafe left/right/none) held, a beam of `beam` states expands every
action each decision and keeps the cheapest by cumulative cost: squared position error (yards) + (YAW_WEIGHT * heading
error in radians)^2 + PRESS_COST per press. **Drift at 1/2/5 s** is the best path's position error that long after
the chunk's anchor; **heading error** its mean absolute facing error. **Distribution distances** are
motion.histogram_distance between the emulated and the human steps' motion.features per context. A chunk is
**expressible** when its 2 s drift is within EXPRESSIBLE_YARDS and its heading error within EXPRESSIBLE_DEGREES.

**Action spaces** evaluated (`SPACES`): `controller` (the move block at the 250 ms decision) and `controller_125ms`
(the same at a 125 ms decision, for the decision-cadence question).
"""

from __future__ import annotations

import datetime as dt
import json
import math
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from animus.human import motion

# PlayerController.h / MoveControls.h
JUMP_SPEED_Z = 7.9555473
GRAVITY = 19.2911053
JUMP_SECONDS = 2.0 * JUMP_SPEED_Z / GRAVITY
DIAGONAL = 0.7071068
VERTICAL_SHARE = 0.7071068
PITCH_LIMIT = math.pi / 2
WALK_RATIO = 2.5 / 7.0          # walk / run (Unit.cpp baseMoveSpeed)
# Back speed over forward speed per mode (Unit.cpp baseMoveSpeed): run_back/run, swim_back/swim, flight_back/flight.
BACK_RATIO = {motion.MODE_GROUND: 4.5 / 7.0, motion.MODE_SWIM: 2.5 / 4.722222, motion.MODE_FLY: 4.5 / 7.0,
              motion.MODE_AIRBORNE: 4.5 / 7.0}
TURN_RATES_DEG = (-360.0, -180.0, -90.0, -30.0, 0.0, 30.0, 90.0, 180.0, 360.0)
PITCH_RATES_DEG = (-90.0, -30.0, 0.0, 30.0, 90.0)

CHUNK_SECONDS = 5.0
YAW_WEIGHT = 2.0                # yards per radian of heading error in the cost
PRESS_COST = 1e-4
EXPRESSIBLE_YARDS = 1.0
EXPRESSIBLE_DEGREES = 10.0
DIST_FEATURES = ("yaw_rate", "planar", "accel", "fwd", "lat")

# Action kinds: the held control an action sets, and the value it sets it to.
NOOP, FORWARD, STRAFE, TURN, PITCH, VERTICAL, JUMP, WALK = range(8)
KIND_NAMES = ("noop", "forward", "strafe", "turn", "pitch", "vertical", "jump", "walk")
#: The feet's start states a chunk is anchored with: (forward, strafe).
FEET = tuple((f, st) for f in (0, 1, -1) for st in (0, -1, 1))


@dataclass(frozen=True)
class Space:
    """An action space: its actions as (kind, value) and its decision interval. Action i + 1 is the move block's local
    action i (MoveControls.h's order); action 0 is no press."""

    name: str
    actions: tuple[tuple[int, float], ...]
    dt: float = motion.DECISION_SECONDS

    def label(self, index: int) -> str:
        kind, value = self.actions[index]
        if kind == FORWARD:
            return {1.0: "move_forward", -1.0: "move_back", 0.0: "move_stop"}[value]
        if kind == STRAFE:
            return {-1.0: "strafe_left", 1.0: "strafe_right", 0.0: "strafe_stop"}[value]
        if kind == VERTICAL:
            return {1.0: "ascend", -1.0: "descend", 0.0: "vertical_stop"}[value]
        if kind == TURN:
            degrees = round(math.degrees(value))
            return "turn_stop" if degrees == 0 else f"turn_{'left' if degrees > 0 else 'right'}_{abs(degrees)}"
        if kind == PITCH:
            degrees = round(math.degrees(value))
            return "pitch_stop" if degrees == 0 else f"pitch_{'up' if degrees > 0 else 'down'}_{abs(degrees)}"
        if kind == WALK:
            return "walk_toggle"
        return KIND_NAMES[kind]


def _controller() -> tuple:
    """No press, then MoveControls.h's 25 actions in their order."""
    acts = [(NOOP, 0.0), (FORWARD, 1.0), (FORWARD, -1.0), (FORWARD, 0.0), (STRAFE, -1.0), (STRAFE, 1.0),
            (STRAFE, 0.0)]
    acts += [(TURN, math.radians(r)) for r in TURN_RATES_DEG]
    acts += [(PITCH, math.radians(r)) for r in PITCH_RATES_DEG]
    acts += [(VERTICAL, 1.0), (VERTICAL, -1.0), (VERTICAL, 0.0), (JUMP, 0.0), (WALK, 0.0)]
    return tuple(acts)


SPACES = {
    "controller": Space("controller", _controller()),
    "controller_125ms": Space("controller_125ms", _controller(), dt=0.125),
}


@dataclass
class State:
    """A batch of emulator states (every field an array of the batch's length)."""

    x: np.ndarray
    y: np.ndarray
    z: np.ndarray
    facing: np.ndarray
    pitch: np.ndarray
    forward: np.ndarray
    strafe: np.ndarray
    turn: np.ndarray            # rad/s
    pitch_rate: np.ndarray
    vertical: np.ndarray
    walk: np.ndarray
    jump_left: np.ndarray       # seconds of a jump still in the air
    jump_vx: np.ndarray
    jump_vy: np.ndarray

    @staticmethod
    def start(n: int, x: float, y: float, z: float, facing: float, pitch: float = 0.0,
              feet: tuple[int, int] | np.ndarray = (0, 0)) -> "State":
        f = lambda v: np.full(n, v, dtype=np.float64)  # noqa: E731
        held = np.broadcast_to(np.asarray(feet, dtype=np.int64).reshape(-1, 2), (n, 2))
        return State(f(x), f(y), f(z), f(facing), f(pitch), held[:, 0].astype(np.float64).copy(),
                     held[:, 1].astype(np.float64).copy(), f(0.0), f(0.0), f(0.0), np.zeros(n, dtype=bool),
                     f(0.0), f(0.0), f(0.0))

    def take(self, index: np.ndarray) -> "State":
        return State(*(getattr(self, name)[index] for name in self.__dataclass_fields__))


def step(state: State, kinds: np.ndarray, values: np.ndarray, mode: int, speed: float, space: Space
         ) -> tuple[State, np.ndarray]:
    """One decision for a batch: the action (kind, value) of each state, then the decision's motion. Returns the new
    states and which actions were allowed (MoveControls::Allowed: pitch rates, ascend and descend only swimming or
    flying, the stops everywhere; jump not mid-jump nor flying)."""
    s = State(*(getattr(state, name).copy() for name in state.__dataclass_fields__))
    n = len(kinds)
    steered = mode in (motion.MODE_SWIM, motion.MODE_FLY)
    jumping = s.jump_left > 1e-9
    allowed = np.ones(n, dtype=bool)
    allowed[(kinds == PITCH) & (values != 0.0) & (not steered)] = False
    allowed[(kinds == VERTICAL) & (values != 0.0) & (not steered)] = False
    allowed[(kinds == JUMP) & (jumping | (mode in (motion.MODE_FLY, motion.MODE_AIRBORNE)))] = False

    for kind, field_name in ((FORWARD, "forward"), (STRAFE, "strafe"), (TURN, "turn"), (PITCH, "pitch_rate"),
                             (VERTICAL, "vertical")):
        mask = kinds == kind
        getattr(s, field_name)[mask] = values[mask]
    toggle = kinds == WALK
    s.walk[toggle] = ~s.walk[toggle]
    if not steered:
        s.pitch[:] = 0.0

    # The turn (and pitch) over the decision, and the heading half-way through it.
    before = s.facing.copy()
    s.facing = s.facing + s.turn * space.dt
    if steered:
        s.pitch = np.clip(s.pitch + s.pitch_rate * space.dt, -PITCH_LIMIT, PITCH_LIMIT)
    heading = before + s.turn * space.dt / 2.0

    # The wish: forward/back and strafe in the body frame, the client's speed rules.
    back = s.forward < 0
    speed_now = np.where(back, speed * BACK_RATIO.get(mode, 1.0), speed)
    speed_now = np.where(s.walk, np.minimum(speed_now, speed * WALK_RATIO), speed_now)
    moving = (s.forward != 0) | (s.strafe != 0)
    diagonal = (s.forward != 0) & (s.strafe != 0)
    fwd = s.forward * np.where(diagonal, DIAGONAL, 1.0)
    side = -s.strafe * np.where(diagonal, DIAGONAL, 1.0)            # strafe right is clockwise: -left
    vx = (fwd * np.cos(heading) - side * np.sin(heading)) * speed_now * moving
    vy = (fwd * np.sin(heading) + side * np.cos(heading)) * speed_now * moving
    vz = np.zeros(n)
    if steered:
        vx = vx * np.cos(s.pitch)
        vy = vy * np.cos(s.pitch)
        vz = fwd * np.sin(s.pitch) * speed_now * moving
        climbing = s.vertical != 0
        share = np.where(climbing & moving, VERTICAL_SHARE, 1.0)
        vx, vy, vz = vx * share, vy * share, vz * share + s.vertical * VERTICAL_SHARE * speed

    # A jump freezes the horizontal velocity it was launched with for its time in the air.
    launch = (kinds == JUMP) & allowed
    s.jump_left[launch] = JUMP_SECONDS
    s.jump_vx[launch] = vx[launch]
    s.jump_vy[launch] = vy[launch]
    air = s.jump_left > 1e-9
    held_time = np.minimum(s.jump_left, space.dt)
    vx = np.where(air, s.jump_vx, vx)
    vy = np.where(air, s.jump_vy, vy)
    s.x += np.where(air, vx * held_time, vx * space.dt)
    s.y += np.where(air, vy * held_time, vy * space.dt)
    s.z += vz * space.dt
    s.jump_left = np.maximum(0.0, s.jump_left - space.dt)
    s.facing = np.remainder(s.facing, 2 * math.pi)
    return s, allowed


def rollout(space: Space, actions: list[int], start: dict, modes: np.ndarray, speeds: np.ndarray) -> np.ndarray:
    """Emulate one action sequence from `start` (x, y, z, facing, pitch, feet = (forward, strafe)): [len(actions) +
    1, 5] rows of x, y, z, facing, pitch."""
    s = State.start(1, start["x"], start["y"], start.get("z", 0.0), start["facing"], start.get("pitch", 0.0),
                    start.get("feet", (0, 0)))
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
    start_feet: tuple[int, int]
    path: np.ndarray                # [K + 1, 5] x, y, z, facing, pitch
    pos_err: np.ndarray             # [K + 1]
    yaw_err: np.ndarray             # [K + 1] radians, absolute
    cost: float


def beam_fit(space: Space, human: np.ndarray, beam: int = 32, start_feet: tuple[tuple[int, int], ...] | None = None,
             jumps: np.ndarray | None = None) -> FitResult:
    """The action sequence of `space` that best reproduces `human` ([K + 1, SAMPLE_DIM] on space.dt), started at
    the human's first sample with any of the feet's states held (FEET), or only `start_feet` when the start is known.
    `jumps` ([K] bool, from the MSG_MOVE_JUMP packets) pins the jumps: JUMP where the human jumped (when a jump can
    be taken there) and nowhere else -- on the ground a jump's carry is otherwise a forward walk's."""
    h = np.asarray(human, dtype=np.float64)
    k_steps = len(h) - 1
    kinds = np.array([a[0] for a in space.actions])
    values = np.array([a[1] for a in space.actions], dtype=np.float64)
    n_act = len(kinds)
    starts = np.asarray(start_feet if start_feet is not None else FEET, dtype=np.int64).reshape(-1, 2)
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
    start_feet = (int(starts[idx][0]), int(starts[idx][1]))
    path = rollout(space, actions, {"x": h[0, motion.X], "y": h[0, motion.Y], "z": h[0, motion.Z],
                                    "facing": h[0, motion.YAW], "pitch": h[0, motion.PITCH],
                                    "feet": start_feet}, h[:, motion.MODE], h[:, motion.SPEED])
    ground = ~np.isin(h[:, motion.MODE].astype(int), (motion.MODE_SWIM, motion.MODE_FLY))
    path[ground, 2] = h[ground, motion.Z]
    pos_err = np.linalg.norm(path[:, :3] - h[:, motion.X:motion.Z + 1], axis=1)
    yaw_err = np.abs(motion.wrap(path[:, 3] - h[:, motion.YAW]))
    return FitResult(actions, start_feet, path, pos_err, yaw_err, float(total[best]) if len(total) else math.inf)


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
                          "within a decision the body moves on the mid-turn heading (a steady turn's chord)",
                          "the human's turns quantised to the policy's nine held rates, a decision at least"],
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
