"""Movement packets -> per-player kinematic tracks on the decision grid (FORMAT.md §3, `motion.SAMPLE`).

One hour shard of one map's move stream at a time (`build_tracks`), joined with the same shard's snapshots (combat,
mount) and outcomes (combat when there are no snapshots), and the session stream for who the player is. Per player:

1. Each Move packet becomes a raw sample: position, orientation, pitch; **mode** from its movement flags (FLYING
   before SWIMMING, which the client sets alongside it; FALLING / FALLING_FAR airborne only when neither); **mounted**
   from MotionEvent mount/dismount, or the snapshot's mounted flag; **speed** from the latest Speeds record for the
   mode (run, or walk under WALKING; swim; flight; a jump or fall carries the run speed), the core's base speeds
   (Unit.cpp baseMoveSpeed) until one arrives; **in_combat** from the latest snapshot's combat flag, or, for a
   player with no snapshots, damage dealt or taken within the last COMBAT_LINGER seconds.
2. **Cuts**: nothing between a cutting event and its end (taxi, death..resurrect, loading screen, vehicle,
   transport) is kept, a teleport cuts where it happens, and so do a map change and a packet on a transport
   (ONTRANSPORT: its position is transport-relative). Tracks are also cut at the end of the shard (the hour).
3. **Involuntary** spans -- root..unroot, stun, fear/confuse, and a knockback until the next packet on its feet
   (at most KNOCKBACK_MAX) -- are kept in the track but flagged per grid sample, for the dataset to leave out.
4. **Standing still**: a client sends nothing while it stands, so the gap after a packet with no movement key held
   would cut the track (`motion.resample` cuts at gaps over `max_gap`) and every stop would end a clip. Such a
   gap is filled with held samples every HOLD_STEP -- only when the player really stood: no movement flag on the
   packet before, the packet after within HOLD_DRIFT yards of it, and the gap at most `hold_max` seconds (longer
   is idle, and cut). A knockback, a fall or an unlogged teleport is therefore never smeared into a glide. This is
   preparation of the packets, not a feature: `motion.resample` and `motion.features` are used as they are.
5. `motion.resample` puts each piece on the grid; held flags and jump packets are carried per grid sample.

Times are server receive times (`ms`), the clock every other stream is on. The client's own clock (`client_ms`)
would time the packets more truly under lag, but would have to be mapped back for every join; latency is handled
by down-weighting high-latency sessions instead (dataset.py).
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from animus.human import motion
from animus.human import reader as r

# Movement flags (src/server/game/Entities/Unit/UnitDefines.h).
MF_FORWARD, MF_BACKWARD, MF_STRAFE_LEFT, MF_STRAFE_RIGHT = 0x1, 0x2, 0x4, 0x8
MF_LEFT, MF_RIGHT, MF_PITCH_UP, MF_PITCH_DOWN = 0x10, 0x20, 0x40, 0x80
MF_WALKING, MF_ONTRANSPORT, MF_ROOT = 0x100, 0x200, 0x800
MF_FALLING, MF_FALLING_FAR = 0x1000, 0x2000
MF_SWIMMING, MF_ASCENDING, MF_DESCENDING = 0x200000, 0x400000, 0x800000
MF_CAN_FLY, MF_FLYING = 0x01000000, 0x02000000
MF_MOVING = (MF_FORWARD | MF_BACKWARD | MF_STRAFE_LEFT | MF_STRAFE_RIGHT | MF_LEFT | MF_RIGHT | MF_PITCH_UP
             | MF_PITCH_DOWN | MF_FALLING | MF_FALLING_FAR | MF_ASCENDING | MF_DESCENDING)
MF_KEYS = MF_FORWARD | MF_BACKWARD | MF_STRAFE_LEFT | MF_STRAFE_RIGHT

# Client opcodes (src/server/game/Server/Protocol/Opcodes.h).
MSG_MOVE_START_FORWARD, MSG_MOVE_STOP, MSG_MOVE_JUMP, MSG_MOVE_FALL_LAND = 0x0B5, 0x0B7, 0x0BB, 0x0C9

# The core's base speeds (Unit.cpp baseMoveSpeed), until a Speeds record says otherwise.
BASE_SPEEDS = {"walk": 2.5, "run": 7.0, "run_back": 4.5, "swim": 4.722222, "swim_back": 2.5, "flight": 7.0,
               "flight_back": 4.5, "turn_rate": 3.141594, "pitch_rate": 3.14}

HOLD_STEP = 0.5             # seconds between held samples while standing
HOLD_MAX = 10.0             # longest stand filled; a longer one is idle and cuts the track
HOLD_DRIFT = 0.5            # yards the next packet may be from the last for the gap to count as standing
KNOCKBACK_MAX = 3.0         # seconds a knockback is involuntary at most
COMBAT_LINGER = 6.0         # seconds after damage a player with no snapshots counts as in combat
SNAPSHOT_STALE = 3.0        # seconds a snapshot's flags are trusted for

# Event pairs whose span is cut (start -> end), and point cuts.
CUT_SPANS = ((r.EV_TAXI_START, r.EV_TAXI_END), (r.EV_DEATH, r.EV_RESURRECT),
             (r.EV_LOADING_START, r.EV_LOADING_END), (r.EV_VEHICLE_ENTER, r.EV_VEHICLE_EXIT),
             (r.EV_TRANSPORT_BOARD, r.EV_TRANSPORT_LEAVE))
CUT_POINTS = (r.EV_TELEPORT,)
INVOLUNTARY_SPANS = ((r.EV_ROOT, r.EV_UNROOT), (r.EV_STUN_START, r.EV_STUN_END),
                     (r.EV_FEAR_START, r.EV_FEAR_END))


def flag_mode(flags: np.ndarray) -> np.ndarray:
    """motion.MODE_* from movement flags."""
    flags = np.asarray(flags, dtype=np.uint32)
    mode = np.full(flags.shape, motion.MODE_GROUND, dtype=np.int64)
    mode[(flags & (MF_FALLING | MF_FALLING_FAR)) != 0] = motion.MODE_AIRBORNE
    mode[(flags & MF_SWIMMING) != 0] = motion.MODE_SWIM
    mode[(flags & MF_FLYING) != 0] = motion.MODE_FLY
    return mode


@dataclass
class Track:
    """One unbroken stretch of one player on the decision grid."""

    player: int
    map: int
    samples: np.ndarray             # [T, motion.SAMPLE_DIM], t in server seconds
    involuntary: np.ndarray         # [T] bool
    flags: np.ndarray               # [T] u32, the movement flags held at each grid time
    jumps: np.ndarray               # [T] i32, MSG_MOVE_JUMP packets in (t - step, t]
    companion: bool = False
    latency_ms: float = 0.0
    hour: str = ""
    info: dict = field(default_factory=dict)    # class_, level, tree_points, kind ... (SessionTable.info)

    @property
    def seconds(self) -> float:
        return float(self.samples[-1, motion.T] - self.samples[0, motion.T]) if len(self.samples) else 0.0


def _spans(times: np.ndarray, kinds: np.ndarray, start: int, end: int, horizon: float) -> list[tuple[float, float]]:
    """[start, end] spans from paired events; a start with no end runs to `horizon`."""
    out = []
    starts = times[kinds == start]
    ends = times[kinds == end]
    for t in starts:
        later = ends[ends >= t]
        out.append((float(t), float(later[0]) if len(later) else horizon))
    return out


def _in_spans(t: np.ndarray, spans: list[tuple[float, float]]) -> np.ndarray:
    inside = np.zeros(len(t), dtype=bool)
    for a, b in spans:
        inside |= (t >= a) & (t <= b)
    return inside


def _held(times: np.ndarray, at: np.ndarray) -> np.ndarray:
    """Index of the latest of `times` at or before each of `at` (-1 when none)."""
    return np.searchsorted(times, at, side="right") - 1


class SessionTable:
    """Who each player is: their latest SessionStart / SessionContext and latency, advanced hour by hour so a
    range of any length is held as one row per player."""

    def __init__(self):
        self.rows: dict[int, np.void] = {}
        self.latency: dict[int, float] = {}
        self.companions: set[int] = set()

    def advance(self, batch: r.Batch) -> None:
        sessions = np.concatenate([batch.get(r.SESSION_START), batch.get(r.SESSION_CONTEXT)])
        for row in sessions[np.argsort(sessions["ms"], kind="stable")]:
            player = int(row["player"])
            self.rows[player] = row
            self.latency[player] = float(row["latency_ms"])
            if int(row["kind"]) == 1:
                self.companions.add(player)
        lat = batch.get(r.LATENCY)
        for row in lat[np.argsort(lat["ms"], kind="stable")]:
            self.latency[int(row["player"])] = float(row["latency_ms"])

    def info(self, player: int) -> dict:
        row = self.rows.get(player)
        if row is None:
            return {}
        return {"class_": int(row["class_"]), "race": int(row["race"]), "level": int(row["level"]),
                "tree_points": [int(v) for v in row["tree_points"]], "item_level": int(row["item_level"]),
                "kind": int(row["kind"]), "session": int(row["session"]), "zone": int(row["zone"]),
                "area": int(row["area"])}

    def is_companion(self, player: int) -> bool:
        return player in self.companions


class CombatIndex:
    """In combat and mounted, per player and time, from one shard's snapshots (Self flags) and, for players with
    none, its outcome records."""

    def __init__(self, snapshots: np.ndarray | None = None, damage: np.ndarray | None = None):
        self.snap: dict[int, tuple[np.ndarray, np.ndarray]] = {}
        self.hits: dict[int, np.ndarray] = {}
        if snapshots is not None and len(snapshots):
            order = np.argsort(snapshots["ms"], kind="stable")
            snaps = snapshots[order]
            for player in np.unique(snaps["player"]):
                mine = snaps[snaps["player"] == player]
                self.snap[int(player)] = (mine["ms"].astype(np.float64) / 1000.0, mine["flags"].astype(np.int64))
        if damage is not None and len(damage):
            times = damage["ms"].astype(np.float64) / 1000.0
            both = np.concatenate([damage["source"], damage["target"]])
            when = np.concatenate([times, times])
            order = np.argsort(both, kind="stable")
            both, when = both[order], when[order]
            bounds = np.flatnonzero(np.diff(both)) + 1
            for ids, ts in zip(np.split(both, bounds), np.split(when, bounds)):
                if len(ids):
                    self.hits[int(ids[0])] = np.sort(ts)

    def flags(self, player: int, t: np.ndarray, bit: int) -> np.ndarray | None:
        found = self.snap.get(player)
        if found is None:
            return None
        times, flags = found
        idx = _held(times, t)
        ok = (idx >= 0) & (t - times[np.maximum(idx, 0)] <= SNAPSHOT_STALE)
        return ok & ((flags[np.maximum(idx, 0)] & bit) != 0)

    def in_combat(self, player: int, t: np.ndarray) -> np.ndarray:
        from_snap = self.flags(player, t, r.SELF_COMBAT)
        if from_snap is not None:
            return from_snap
        hits = self.hits.get(player)
        if hits is None:
            return np.zeros(len(t), dtype=bool)
        idx = _held(hits, t)
        return (idx >= 0) & (t - hits[np.maximum(idx, 0)] <= COMBAT_LINGER)


def _speeds_at(speeds: np.ndarray, t: np.ndarray, column: str) -> np.ndarray:
    if len(speeds) == 0:
        return np.full(len(t), BASE_SPEEDS[column])
    times = speeds["ms"].astype(np.float64) / 1000.0
    idx = _held(times, t)
    values = speeds[column].astype(np.float64)
    out = np.where(idx >= 0, values[np.maximum(idx, 0)], values[0])
    return np.where(out > 0.1, out, BASE_SPEEDS[column])


def player_tracks(player: int, moves: np.ndarray, events: np.ndarray, speeds: np.ndarray,
                  combat: CombatIndex | None = None, step: float = motion.DECISION_SECONDS,
                  max_gap: float = 1.5, hold_max: float = HOLD_MAX) -> list[Track]:
    """The tracks of one player from their Move, MotionEvent and Speeds records (any order)."""
    moves = moves[np.argsort(moves["ms"], kind="stable")]
    events = events[np.argsort(events["ms"], kind="stable")]
    speeds = speeds[np.argsort(speeds["ms"], kind="stable")]
    if len(moves) < 2:
        return []
    t = moves["ms"].astype(np.float64) / 1000.0
    flags = moves["move_flags"].astype(np.int64)
    horizon = float(t[-1]) + 1.0
    ev_t = events["ms"].astype(np.float64) / 1000.0
    ev_k = events["event"].astype(np.int64)

    cut_spans = [s for a, b in CUT_SPANS for s in _spans(ev_t, ev_k, a, b, horizon)]
    cut_points = [float(x) for x in ev_t[np.isin(ev_k, CUT_POINTS)]]
    invol = [s for a, b in INVOLUNTARY_SPANS for s in _spans(ev_t, ev_k, a, b, horizon)]
    for tk in ev_t[ev_k == r.EV_KNOCKBACK]:
        after = np.flatnonzero((t > tk) & ((flags & (MF_FALLING | MF_FALLING_FAR)) == 0))
        invol.append((float(tk), float(min(t[after[0]], tk + KNOCKBACK_MAX)) if len(after) else tk + KNOCKBACK_MAX))

    keep = ~_in_spans(t, cut_spans) & ((flags & MF_ONTRANSPORT) == 0)
    # Split points: between packets where a cut point or a span edge falls, a map changes, or a dropped packet sat.
    bound = np.zeros(len(t), dtype=bool)        # True: a new piece starts at packet i
    edges = cut_points + [a for a, _ in cut_spans] + [b for _, b in cut_spans]
    if edges:
        edge_idx = np.searchsorted(t, np.asarray(edges), side="right")
        bound[edge_idx[edge_idx < len(t)]] = True
    bound[1:] |= moves["map"][1:] != moves["map"][:-1]
    dropped = ~keep
    bound[1:] |= dropped[:-1]

    mounted_ev = np.zeros(len(t), dtype=bool)
    mounts = [s for s in _spans(ev_t, ev_k, r.EV_MOUNT, r.EV_DISMOUNT, horizon)]
    mounted_ev |= _in_spans(t, mounts)
    if combat is not None:
        snap_mount = combat.flags(player, t, r.SELF_MOUNTED)
        if snap_mount is not None:
            mounted_ev |= snap_mount
    mode = flag_mode(flags)
    walking = (flags & MF_WALKING) != 0
    speed = np.where(walking, _speeds_at(speeds, t, "walk"), _speeds_at(speeds, t, "run"))
    speed = np.where(mode == motion.MODE_SWIM, _speeds_at(speeds, t, "swim"), speed)
    speed = np.where(mode == motion.MODE_FLY, _speeds_at(speeds, t, "flight"), speed)
    combat_flag = combat.in_combat(player, t) if combat is not None else np.zeros(len(t), dtype=bool)
    jump_t = t[moves["opcode"] == MSG_MOVE_JUMP]

    raw = np.zeros((len(t), motion.SAMPLE_DIM))
    raw[:, motion.T] = t
    raw[:, motion.X] = moves["x"]
    raw[:, motion.Y] = moves["y"]
    raw[:, motion.Z] = moves["z"]
    raw[:, motion.YAW] = moves["o"]
    raw[:, motion.PITCH] = moves["pitch"]
    raw[:, motion.MODE] = mode
    raw[:, motion.MOUNTED] = mounted_ev
    raw[:, motion.SPEED] = speed
    raw[:, motion.IN_COMBAT] = combat_flag

    out: list[Track] = []
    starts = np.flatnonzero(bound | (np.arange(len(t)) == 0))
    for a, b in zip(starts, list(starts[1:]) + [len(t)]):
        idx = np.arange(a, b)
        idx = idx[keep[idx]]
        if len(idx) < 2:
            continue
        for piece in _fill_and_split(raw[idx], flags[idx], max_gap, hold_max):
            raw_piece, flag_piece = piece
            grids = motion.resample(raw_piece, step=step, max_gap=max_gap)
            for grid in grids:
                gt = grid[:, motion.T]
                held = np.clip(_held(raw_piece[:, motion.T], gt), 0, len(raw_piece) - 1)
                jumps = (np.searchsorted(jump_t, gt, side="right")
                         - np.searchsorted(jump_t, gt - step, side="right")).astype(np.int32)
                out.append(Track(player=player, map=int(moves["map"][idx[0]]), samples=grid,
                                 involuntary=_in_spans(gt, invol), flags=flag_piece[held].astype(np.uint32),
                                 jumps=jumps))
    return out


def _fill_and_split(raw: np.ndarray, flags: np.ndarray, max_gap: float, hold_max: float
                    ) -> list[tuple[np.ndarray, np.ndarray]]:
    """Fill the gaps a standing player leaves with held samples; split at every other gap over `max_gap`."""
    t = raw[:, motion.T]
    gaps = np.flatnonzero(np.diff(t) > max_gap)
    if len(gaps) == 0:
        return [(raw, flags)]
    pieces: list[tuple[np.ndarray, np.ndarray]] = []
    rows: list[np.ndarray] = []
    flag_rows: list[np.ndarray] = []
    last = 0
    for g in gaps:
        rows.append(raw[last:g + 1])
        flag_rows.append(flags[last:g + 1])
        gap = t[g + 1] - t[g]
        drift = float(np.linalg.norm(raw[g + 1, motion.X:motion.Z + 1] - raw[g, motion.X:motion.Z + 1]))
        if (flags[g] & MF_MOVING) == 0 and drift <= HOLD_DRIFT and gap <= hold_max:
            fill_t = np.arange(t[g] + HOLD_STEP, t[g + 1] - 1e-6, HOLD_STEP)
            hold = np.repeat(raw[g:g + 1], len(fill_t), axis=0)
            hold[:, motion.T] = fill_t
            rows.append(hold)
            flag_rows.append(np.repeat(flags[g:g + 1], len(fill_t)))
        else:
            pieces.append((np.concatenate(rows), np.concatenate(flag_rows)))
            rows, flag_rows = [], []
        last = g + 1
    rows.append(raw[last:])
    flag_rows.append(flags[last:])
    pieces.append((np.concatenate(rows), np.concatenate(flag_rows)))
    return [p for p in pieces if len(p[0]) >= 2]


def build_tracks(moves_batch: r.Batch, combat: CombatIndex | None = None, sessions: SessionTable | None = None,
                 hour: str = "", step: float = motion.DECISION_SECONDS, include_companions: bool = False,
                 max_gap: float = 1.5, hold_max: float = HOLD_MAX) -> list[Track]:
    """Every player's tracks from one move shard. Companions -- a session of kind 1, or packets synthesised from
    a companion's server position (source 1) -- are left out unless `include_companions`, and tagged when kept:
    nothing named human_* may learn from the bots it judges."""
    moves = moves_batch.get(r.MOVE)
    events = moves_batch.get(r.MOTION_EVENT)
    speeds = moves_batch.get(r.SPEEDS)
    out: list[Track] = []
    if len(moves) == 0:
        return out
    order = np.argsort(moves["player"], kind="stable")
    moves = moves[order]
    bounds = np.flatnonzero(np.diff(moves["player"])) + 1
    for mine in np.split(moves, bounds):
        player = int(mine["player"][0])
        companion = bool((mine["source"] == 1).any()) or (sessions is not None and sessions.is_companion(player))
        if companion and not include_companions:
            continue
        tracks = player_tracks(player, mine, events[events["player"] == player], speeds[speeds["player"] == player],
                               combat, step=step, max_gap=max_gap, hold_max=hold_max)
        for track in tracks:
            track.companion = companion
            track.hour = hour
            if sessions is not None:
                track.latency_ms = sessions.latency.get(player, 0.0)
                track.info = sessions.info(player)
        out.extend(tracks)
    return out


@dataclass
class Shard:
    """One hour shard of one map: its move records and what the other streams say about the same players."""

    hour: r.HourDir
    shard: str
    moves: r.Batch
    combat: CombatIndex
    actions: r.Batch
    outcomes: r.Batch


def shard_path(hour: r.HourDir, stream: str, shard: str):
    for path in hour.files(stream):
        if r.stream_map(path)[1] == shard:
            return path
    return None


SNAPSHOT_FLAGS = np.dtype([("ms", "<u8"), ("player", "<u8"), ("flags", "u1")])


def _snapshot_flags(snapshots: np.ndarray) -> np.ndarray:
    """Just the time, player and Self flags of snapshots: what combat and mount need, a tenth of the prefix."""
    out = np.zeros(len(snapshots), SNAPSHOT_FLAGS)
    for name in SNAPSHOT_FLAGS.names:
        out[name] = snapshots[name]
    return out


def iter_shards(cap: r.CaptureDir, sessions: SessionTable | None = None):
    """(SessionTable advanced to the hour, Shard) for every move shard in the range, hour by hour. Only one shard's
    records are held at a time (plus a row per player in the session table)."""
    sessions = sessions if sessions is not None else SessionTable()
    for hour in cap.hours():
        for path in hour.files("session"):
            for batch in r.read_file(path):
                sessions.advance(batch)
        for move_path in hour.files("move"):
            shard = r.stream_map(move_path)[1]
            moves = r.read_all(move_path)
            snap_path = shard_path(hour, "snapshot", shard)
            out_path = shard_path(hour, "outcome", shard)
            act_path = shard_path(hour, "action", shard)
            snapshots = None
            if snap_path is not None:
                parts = [_snapshot_flags(b.get(r.SNAPSHOT)) for b in r.read_file(snap_path)]
                snapshots = np.concatenate(parts) if parts else None
            outcomes = r.read_all(out_path) if out_path is not None else r.Batch()
            actions = r.read_all(act_path) if act_path is not None else r.Batch()
            combat = CombatIndex(snapshots, outcomes.get(r.DAMAGE))
            yield sessions, Shard(hour, shard, moves, combat, actions, outcomes)
