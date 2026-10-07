"""Tracks and events -> what training uses: motion clips, trips, fights, deaths and corpse runs, idle stretches.

**Clips** are the unit the style dataset learns from: a stretch of one track with no involuntary motion and no idle
stretch in it, tagged with its context -- terrain where the data says it (`water` swimming, `air` flying, `ground`
otherwise; `indoors` is not derivable from the capture and is left None for a later pass against the forge's
maps), the share mounted and in combat, and its purpose by hindsight (`fight`, `trip`, or `other`).

**Trips** have hindsight destinations: from where a player was (a track's start or the end of a stay) to the next
place they *stopped and stayed* (STAY_SECONDS within STAY_RADIUS) or *interacted* (an Interact record: a vendor,
a flight master, a quest giver ...). A track that ends standing still ended in a stay; one cut by a taxi or a death
has no destination and gives no trip.

**Fights** run from the first damage the player dealt or took to the last, gaps under FIGHT_GAP joined. **Deaths**
are Death records; a **corpse run** is the ghost's way from a death to its resurrection, measured on the raw
packets (the track builder cuts it out of the tracks: it is not player style to learn from).

**Idle**: a stretch of IDLE_SECONDS or more with no movement and no turning is AFK, never a clip.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

from animus.human import motion
from animus.human import reader as r
from animus.human.tracks import MF_KEYS, MF_MOVING, Track

STAY_SECONDS = 5.0          # standing this long somewhere is a destination
STAY_RADIUS = 3.0           # yards the body may drift while staying
MIN_TRIP_YARDS = 15.0       # a trip goes somewhere
MIN_TRIP_SECONDS = 3.0
PATH_EVERY = 8              # grid samples between recorded trip path points (2 s)
FIGHT_GAP = 8.0             # seconds without damage that end a fight
IDLE_SECONDS = 8.0          # no movement and no turning for this long is idle
IDLE_TURN = math.radians(1.0)
STUCK_SECONDS = 3.0         # keys held this long with the body going nowhere is stuck
STUCK_YARDS = 1.5


@dataclass
class Clip:
    track: Track
    start: int                  # grid sample index, inclusive
    end: int                    # exclusive
    terrain: str = "ground"     # ground / water / air
    indoors: bool | None = None # not derivable from the capture; left for a pass against the maps
    mounted_share: float = 0.0
    combat_share: float = 0.0
    purpose: str = "other"      # trip / fight / other

    @property
    def samples(self) -> np.ndarray:
        return self.track.samples[self.start:self.end]


@dataclass
class Trip:
    player: int
    map: int
    start: list[float]
    end: list[float]
    seconds: float
    mode: str
    path: list[list[float]]
    destination: str            # stay / interact
    t0: float = 0.0
    t1: float = 0.0
    track: Track | None = field(default=None, repr=False)
    span: tuple[int, int] = (0, 0)


def idle_mask(track: Track, seconds: float = IDLE_SECONDS) -> np.ndarray:
    """[T] True on samples inside an idle stretch: no movement (planar under motion.MOVING) and no turn over
    IDLE_TURN between consecutive samples, for `seconds` or more."""
    s = track.samples
    n = len(s)
    if n < 2:
        return np.zeros(n, dtype=bool)
    feats = motion.features(s)
    step = np.diff(s[:, motion.T])
    still = (feats[:, motion.INDEX["planar"]] <= motion.MOVING) \
        & (np.abs(motion.wrap(np.diff(s[:, motion.YAW]))) < IDLE_TURN) \
        & (np.abs(np.diff(s[:, motion.PITCH])) < IDLE_TURN)
    out = np.zeros(n, dtype=bool)
    i = 0
    while i < len(still):
        if not still[i]:
            i += 1
            continue
        j = i
        while j < len(still) and still[j]:
            j += 1
        if step[i:j].sum() >= seconds:
            out[i:j + 1] = True
        i = j
    return out


def _runs(mask: np.ndarray) -> list[tuple[int, int]]:
    """[start, end) of each run of True."""
    m = np.concatenate([[False], np.asarray(mask, dtype=bool), [False]])
    d = np.diff(m.astype(np.int8))
    return list(zip(np.flatnonzero(d == 1), np.flatnonzero(d == -1)))


def clips(track: Track, min_steps: int = motion.WINDOW + 1, trips: list[Trip] | None = None) -> list[Clip]:
    """The clips of a track: runs without involuntary or idle samples, at least `min_steps` samples long."""
    usable = ~track.involuntary & ~idle_mask(track)
    out = []
    s = track.samples
    for a, b in _runs(usable):
        if b - a < min_steps:
            continue
        mode = s[a:b, motion.MODE].astype(int)
        swim, fly = (mode == motion.MODE_SWIM).mean(), (mode == motion.MODE_FLY).mean()
        terrain = "water" if swim >= 0.5 else "air" if fly >= 0.5 else "ground"
        combat = float((s[a:b, motion.IN_COMBAT] > 0.5).mean())
        clip = Clip(track, int(a), int(b), terrain=terrain, mounted_share=float((s[a:b, motion.MOUNTED] > 0.5).mean()),
                    combat_share=combat)
        if combat >= 0.5:
            clip.purpose = "fight"
        elif trips and any(t.track is track and t.span[0] < b and t.span[1] > a for t in trips):
            clip.purpose = "trip"
        out.append(clip)
    return out


def stays(track: Track, seconds: float = STAY_SECONDS, radius: float = STAY_RADIUS) -> list[tuple[int, int]]:
    """[start, end) sample spans where the body stayed within `radius` yards for `seconds` or more."""
    s = track.samples
    n = len(s)
    out = []
    i = 0
    while i < n:
        j = i
        while j + 1 < n and np.linalg.norm(s[j + 1, motion.X:motion.Z + 1] - s[i, motion.X:motion.Z + 1]) <= radius:
            j += 1
        if s[j, motion.T] - s[i, motion.T] >= seconds:
            out.append((i, j + 1))
            i = j + 1
        else:
            i += 1
    return out


def trips(track: Track, interacts: np.ndarray | None = None) -> list[Trip]:
    """Trips of one track, each ending where the player stopped and stayed or interacted."""
    s = track.samples
    n = len(s)
    if n < 2:
        return []
    ends: list[tuple[int, str]] = []
    stay_spans = stays(track)
    for a, _ in stay_spans:
        ends.append((a, "stay"))
    # A track ending standing still (no key held, not moving) ended in a stay the track builder cut as idle.
    last_still = (int(track.flags[-1]) & MF_MOVING) == 0
    if last_still and not any(b == n for _, b in stay_spans):
        ends.append((n - 1, "stay"))
    if interacts is not None and len(interacts):
        mine = interacts[interacts["player"] == track.player]
        for row in mine:
            t = float(row["ms"]) / 1000.0
            if s[0, motion.T] <= t <= s[-1, motion.T]:
                k = int(np.clip(np.searchsorted(s[:, motion.T], t), 0, n - 1))
                ends.append((k, "interact"))
    ends.sort()
    out = []
    origin = _stay_end(stay_spans, 0, 0)
    for end, kind in ends:
        if end > origin:
            trip = _trip(track, origin, end, kind)
            if trip is not None:
                out.append(trip)
        origin = max(origin, _stay_end(stay_spans, end, end))
    return out


def _stay_end(stay_spans: list[tuple[int, int]], at: int, default: int) -> int:
    for a, b in stay_spans:
        if a <= at < b:
            return b - 1
    return default


def _trip(track: Track, a: int, b: int, kind: str) -> Trip | None:
    s = track.samples
    # From where the feet started moving, not from the first sample of the stand before it.
    while a < b and np.linalg.norm(s[a + 1, motion.X:motion.Z + 1] - s[a, motion.X:motion.Z + 1]) < 0.05:
        a += 1
    if track.involuntary[a:b + 1].any():
        return None
    seconds = float(s[b, motion.T] - s[a, motion.T])
    start, end = s[a, motion.X:motion.Z + 1], s[b, motion.X:motion.Z + 1]
    if seconds < MIN_TRIP_SECONDS or np.linalg.norm(end - start) < MIN_TRIP_YARDS:
        return None
    modes = s[a:b + 1, motion.MODE].astype(int)
    mode = ("ground", "swim", "fly", "ground")[int(np.bincount(modes, minlength=4)[:3].argmax())]
    # The trip readers' modes (FORMAT.md §5): a mounted ride on the ground is "mounted"; a flight is "fly" either way.
    if mode == "ground" and (s[a:b + 1, motion.MOUNTED] > 0.5).mean() >= 0.5:
        mode = "mounted"
    idx = list(range(a, b, PATH_EVERY)) + [b]
    path = [[round(float(v), 2) for v in s[i, motion.X:motion.Z + 1]] for i in idx]
    return Trip(player=track.player, map=track.map, start=path[0], end=path[-1], seconds=round(seconds, 2),
                mode=mode, path=path, destination=kind, t0=float(s[a, motion.T]), t1=float(s[b, motion.T]),
                track=track, span=(a, b))


def fights(outcomes: r.Batch, player: int, gap: float = FIGHT_GAP) -> list[dict]:
    """Fights of one player: first damage dealt or taken to the last, with what was dealt, taken and healed."""
    dmg = outcomes.get(r.DAMAGE)
    mine = dmg[(dmg["source"] == player) | (dmg["target"] == player)]
    if len(mine) == 0:
        return []
    mine = mine[np.argsort(mine["ms"], kind="stable")]
    t = mine["ms"].astype(np.float64) / 1000.0
    cuts = np.flatnonzero(np.diff(t) > gap) + 1
    heals = outcomes.get(r.HEAL)
    heals = heals[heals["source"] == player]
    ht = heals["ms"].astype(np.float64) / 1000.0
    out = []
    for part, times in zip(np.split(mine, cuts), np.split(t, cuts)):
        t0, t1 = float(times[0]), float(times[-1])
        dealt = part[part["source"] == player]
        taken = part[part["target"] == player]
        inside = (ht >= t0) & (ht <= t1)
        out.append({"t0": t0, "t1": t1, "seconds": t1 - t0,
                    "dealt": int(dealt["amount"].astype(np.int64).sum()),
                    "taken": int(taken["amount"].astype(np.int64).sum()),
                    "healed": int(heals["amount"][inside].astype(np.int64).sum()),
                    "overheal": int(heals["overheal"][inside].astype(np.int64).sum())})
    return out


DEATH_CAUSES = {0: "creature", 1: "player", 2: "fall", 3: "drowning", 4: "fire", 5: "other"}


def deaths(outcomes: r.Batch, moves: r.Batch | None = None) -> list[dict]:
    """Every Death, with the corpse run to the resurrection that followed (when the shard has it): its seconds and
    the ghost's path length over the raw packets."""
    out = []
    events = moves.get(r.MOTION_EVENT) if moves is not None else np.zeros(0, r.PREFIX[r.MOTION_EVENT][1])
    raw = moves.get(r.MOVE) if moves is not None else np.zeros(0, r.PREFIX[r.MOVE][1])
    for row in outcomes.get(r.DEATH):
        player, t = int(row["player"]), int(row["ms"])
        item = {"player": player, "ms": t, "cause": DEATH_CAUSES.get(int(row["cause"]), "other"),
                "pos": [float(row["x"]), float(row["y"]), float(row["z"])], "map": int(row["map"]),
                "corpse_run": None}
        res = events[(events["player"] == player) & (events["event"] == r.EV_RESURRECT) & (events["ms"] >= t)]
        if len(res):
            t1 = int(res["ms"].min())
            ghost = raw[(raw["player"] == player) & (raw["ms"] > t) & (raw["ms"] < t1)]
            ghost = ghost[np.argsort(ghost["ms"], kind="stable")]
            xyz = np.stack([ghost["x"], ghost["y"], ghost["z"]], axis=1).astype(np.float64)
            length = float(np.linalg.norm(np.diff(xyz, axis=0), axis=1).sum()) if len(xyz) > 1 else 0.0
            item["corpse_run"] = {"seconds": (t1 - t) / 1000.0, "yards": round(length, 2)}
        out.append(item)
    return out


def stuck_spans(track: Track, seconds: float = STUCK_SECONDS, yards: float = STUCK_YARDS) -> list[tuple[int, int]]:
    """[start, end) spans where a movement key was held for `seconds` or more while the body moved under `yards`
    from where the span began: walking into a wall."""
    keys = (track.flags.astype(np.int64) & MF_KEYS) != 0
    s = track.samples
    out = []
    for a, b in _runs(keys & ~track.involuntary):
        i = a
        while i < b:
            j = i
            while j + 1 < b and np.linalg.norm(s[j + 1, motion.X:motion.Y + 1] - s[i, motion.X:motion.Y + 1]) < yards:
                j += 1
            if s[j, motion.T] - s[i, motion.T] >= seconds:
                out.append((int(i), int(j + 1)))
                i = j + 1
            else:
                i += 1
    return out
