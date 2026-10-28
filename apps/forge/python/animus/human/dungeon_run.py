"""A dungeon run read off one visit of a map instance (a span, `spans.py`), party-wide: the packs, the pulls in the
order the party made them, the rests, the door and lever interactions, the bosses, what was skipped, the deaths --
and from those the **clear way** the forge's PointNav stages follow (`AnimusForge.Curriculum.PointNav.ClearWayFile`),
so the bots learn the dungeon as the people ran it rather than a generated route between spawn clusters.

    python -m animus.human dungeon-run --capture <root> [--date D] (--span <index> | --map M [--instance I]
        [--from-ms A --to-ms B]) --out <report.json>

Nothing here is typed by anyone: pulls come from CombatEdge (a creature entering combat with the party), packs from
the map's formations, then co-engagement, then spawn proximity; rests from every player standing still out of
combat; interactions from any player's Interact joined to the object's state change; bosses from the spawn table's
boss flag. Beside the JSON, `<out>.clearway.csv` (`index,kind,x,y,z,spawn_ids`) is what the core loads, and
`<out>.path.csv` one player's recorded line (the one with the most movement packets) leg by leg.
"""

from __future__ import annotations

import csv
import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from animus.human import reader as r
from animus.human.spans import Span

SITE_YARDS = 30.0           # spawns this close (and on one floor) are one pack when nothing else says
SITE_RISE = 6.0
CO_ENGAGE_MS = 2000         # creatures entering combat within this of one another are one pull
PULL_GAP_MS = 3000          # a creature engaging this long after the pull's last, with the party out of combat,
                            # starts a new pull
PATH_STEP_MS = 500          # the recorded line's samples at most this often (a leg's path for the sim)
REST_SPEED = 0.3            # yd/s: standing still
REST_SECONDS = 5.0
DOOR_JOIN_MS = 3000         # an Interact and the object's state change this close are one interaction
PASSED_YARDS = 40.0         # a creature alive at the end that a player came this close to was skipped
GO_DOOR, GO_BUTTON, GO_CHEST, GO_GOOBER = 0, 1, 3, 10
GO_STATE_READY = 1              # a door shut, a chest unopened; 0 open/active, 2 the alternative (a cannon fired)
OPENING_SPELLS = {3365, 6247, 6477, 6478, 21651, 22810}    # the client's Opening casts on a game object
CANNON_ITEM, CANNON_SPELL = 5397, 6250                     # Defias Gunpowder fires the Defias Cannon


@dataclass
class Spawn:
    spawn_id: int
    entry: int
    x: float
    y: float
    z: float
    rank: int
    boss: bool
    leader: int
    kind: int           # 0 creature, 1 game object
    go_type: int


@dataclass
class Run:
    span: Span
    players: list[int] = field(default_factory=list)            # the humans
    party: set[int] = field(default_factory=set)                # humans, companions and their pets
    map: int = 0
    instance: int = 0
    start_ms: int = 0
    end_ms: int = 0
    entrance: tuple[float, float, float] | None = None
    spawns: dict[int, Spawn] = field(default_factory=dict)
    moves: np.ndarray | None = None             # every player's Move rows on the map, by ms
    moves_of: dict[int, np.ndarray] = field(default_factory=dict)
    snapshots_of: dict[int, np.ndarray] = field(default_factory=dict)   # each player's Snapshot rows, by ms
    edges: np.ndarray | None = None             # CombatEdge rows of the instance
    kills: np.ndarray | None = None
    deaths: np.ndarray | None = None
    interacts: np.ndarray | None = None
    go_changes: np.ndarray | None = None        # GameObjectState rows with event changed
    item_uses: np.ndarray | None = None
    casts: np.ndarray | None = None
    alive_at_end: dict[int, bool] = field(default_factory=dict)
    threat_members: dict[int, set[int]] = field(default_factory=dict)   # creature spawn -> units ever on its list
    engaged_ms: dict[int, int] = field(default_factory=dict)            # creature spawn -> first UnitState engaged
    path_player: int = 0                                                 # whose line .path.csv follows


# ---- loading ---------------------------------------------------------------------------------------------------------

def _sorted(rows: np.ndarray) -> np.ndarray:
    return rows[np.argsort(rows["ms"], kind="stable")] if len(rows) else rows


def _inside(rows: np.ndarray, lo: int, hi: int) -> np.ndarray:
    if rows is None or not len(rows):
        return rows
    return rows[(rows["ms"] >= lo) & (rows["ms"] <= hi)]


def _concat(parts: list[np.ndarray], rtype: int) -> np.ndarray:
    parts = [p for p in parts if p is not None and len(p)]
    return np.concatenate(parts) if parts else np.zeros(0, r.PREFIX[rtype][1])


def _map_files(cap: r.CaptureDir, span: Span, stream: str):
    """The batches of one stream's file of the span's map, hour by hour over the span."""
    for hour in span.hours(cap):
        for path in hour.files(stream):
            if r.stream_map(path)[1] == str(span.map):
                yield from r.read_file(path)


def load(cap: r.CaptureDir, span: Span) -> Run:
    """Everything the derivations need, from the map's files within the span's window."""
    run = Run(span)
    run.map = span.map
    run.instance = span.instance
    run.players = list(span.players)
    run.party = set(span.players) | set(span.companions)
    lo, hi = span.window()
    run.start_ms = lo
    run.end_ms = span.end_ms or 0

    # The spawn table (any dump of the map in the span's hours; the hourly dumps are all the same table), who is
    # alive (the last dump of this instance in the window), the players' snapshots, the units' threat lists and
    # engaged flags, the object changes.
    spawn_parts: list[np.ndarray] = []
    alive_parts: dict[int, np.ndarray] = {}
    alive_ms = -1
    snaps: dict[int, list[np.ndarray]] = {}
    go_parts: list[np.ndarray] = []
    pets: set[int] = set()
    for batch in _map_files(cap, span, "snapshot"):
        rows = batch.get(r.MAP_SPAWNS)
        if len(rows) and not spawn_parts:
            first_ms = int(rows[0]["ms"])
            for i, row in enumerate(rows):
                if int(row["map"]) == span.map and int(row["ms"]) == first_ms:
                    spawn_parts.append(batch.tail_rows(r.MAP_SPAWNS, i))
        rows = batch.get(r.INSTANCE_CREATURES)
        for i, row in enumerate(rows):
            if int(row["map"]) != span.map or int(row["instance"]) != span.instance or not lo <= int(row["ms"]) <= hi:
                continue
            if int(row["ms"]) > alive_ms:
                alive_ms = int(row["ms"])
                alive_parts.clear()
            if int(row["ms"]) == alive_ms:
                alive_parts[int(row["part"])] = batch.tail_rows(r.INSTANCE_CREATURES, i)
        rows = batch.get(r.SNAPSHOT)
        if len(rows):
            rows = _inside(rows, lo, hi)
            for player in run.players:
                mine = rows[rows["player"] == player]
                if len(mine):
                    snaps.setdefault(player, []).append(mine)
        units = batch.get(r.UNIT_STATE)
        if len(units):
            units_idx = np.flatnonzero((units["ms"] >= lo) & (units["ms"] <= hi))
            for i in units_idx:
                row = units[i]
                flags = int(row["flags"])
                if int(row["unit_type"]) == 2 and flags & r.US_MY_PET:
                    pets.add(int(row["unit"]))
                if int(row["unit_type"]) != 0 or flags & r.US_GONE:
                    continue
                sid = int(row["spawn_id"])
                if not sid:
                    continue
                if flags & r.US_ENGAGED and sid not in run.engaged_ms:
                    run.engaged_ms[sid] = int(row["ms"])
                if flags & r.US_COMBAT:
                    threats = batch.unit_state_details(int(i))["threats"]
                    if len(threats):
                        run.threat_members.setdefault(sid, set()).update(int(u) for u in threats["unit"])
        changes = batch.get(r.GAME_OBJECT_STATE)
        if len(changes):
            changes = _inside(changes, lo, hi)
            go_parts.append(changes[changes["event"] == r.GO_CHANGED])
    run.party |= pets
    for part in spawn_parts:
        for row in part:
            run.spawns[int(row["spawn_id"])] = Spawn(int(row["spawn_id"]), int(row["entry"]), float(row["x"]),
                                                     float(row["y"]), float(row["z"]), int(row["rank"]),
                                                     bool(row["boss"]), int(row["formation_leader_spawn"]),
                                                     int(row["kind"]), int(row["go_type"]))
    for part in alive_parts.values():
        for row in part:
            run.alive_at_end[int(row["spawn_id"])] = bool(row["alive"])
    for player, parts in snaps.items():
        run.snapshots_of[player] = _sorted(np.concatenate(parts))
    run.go_changes = _sorted(_concat(go_parts, r.GAME_OBJECT_STATE))

    move_parts: list[np.ndarray] = []
    for batch in _map_files(cap, span, "move"):
        rows = batch.get(r.MOVE)
        if len(rows):
            rows = _inside(rows, lo, hi)
            move_parts.append(rows[np.isin(rows["player"], run.players) & (rows["map"] == span.map)])
    moves = _sorted(_concat(move_parts, r.MOVE))
    run.moves = moves
    for player in run.players:
        run.moves_of[player] = moves[moves["player"] == player]
    if len(moves):
        first = moves[0]
        run.entrance = (float(first["x"]), float(first["y"]), float(first["z"]))
        run.path_player = max(run.players, key=lambda p: len(run.moves_of[p]))
    elif run.players:
        run.path_player = run.players[0]

    edge_parts, kill_parts, death_parts = [], [], []
    for batch in _map_files(cap, span, "outcome"):
        edges = batch.get(r.COMBAT_EDGE)
        if len(edges):
            edges = _inside(edges, lo, hi)
            edge_parts.append(edges[(edges["map"] == span.map) & (edges["instance"] == span.instance)])
        kill_parts.append(_inside(batch.get(r.KILL), lo, hi))
        deaths = batch.get(r.DEATH)
        if len(deaths):
            deaths = _inside(deaths, lo, hi)
            death_parts.append(deaths[np.isin(deaths["player"], run.players) & (deaths["map"] == span.map)])
    run.edges = _sorted(_concat(edge_parts, r.COMBAT_EDGE))
    run.kills = _sorted(_concat(kill_parts, r.KILL))
    run.deaths = _sorted(_concat(death_parts, r.DEATH))

    interact_parts, use_parts, cast_parts = [], [], []
    for batch in _map_files(cap, span, "action"):
        for rtype, parts in ((r.INTERACT, interact_parts), (r.ITEM_USE, use_parts), (r.CAST_REQUEST, cast_parts)):
            rows = batch.get(rtype)
            if len(rows):
                rows = _inside(rows, lo, hi)
                parts.append(rows[np.isin(rows["player"], run.players)])
    run.interacts = _sorted(_concat(interact_parts, r.INTERACT))
    run.item_uses = _sorted(_concat(use_parts, r.ITEM_USE))
    run.casts = _sorted(_concat(cast_parts, r.CAST_REQUEST))
    if not run.end_ms and len(run.moves):
        run.end_ms = int(run.moves[-1]["ms"])
    return run


# ---- helpers ---------------------------------------------------------------------------------------------------------

def _at(rows: np.ndarray, ms: int):
    """The last row at or before `ms` (rows sorted by ms); None before the first."""
    if rows is None or not len(rows):
        return None
    k = int(np.searchsorted(rows["ms"], ms, side="right")) - 1
    return rows[k] if k >= 0 else None


def _between(rows: np.ndarray, start: int, end: int) -> np.ndarray:
    if rows is None or not len(rows):
        return rows if rows is not None else np.zeros(0)
    lo = int(np.searchsorted(rows["ms"], start, side="left"))
    hi = int(np.searchsorted(rows["ms"], end, side="right"))
    return rows[lo:hi]


def _in_combat(run: Run, ms: int) -> bool:
    """Any player's latest snapshot at `ms` says combat."""
    for snaps in run.snapshots_of.values():
        snap = _at(snaps, ms)
        if snap is not None and int(snap["flags"]) & r.SELF_COMBAT:
            return True
    return False


def _xyz(row) -> list[float]:
    return [round(float(row["x"]), 2), round(float(row["y"]), 2), round(float(row["z"]), 2)]


def _median_spot(moves: np.ndarray) -> list[float] | None:
    if moves is None or not len(moves):
        return None
    return [round(float(np.median(moves[c])), 2) for c in ("x", "y", "z")]


def _mean_pct(run: Run, ms: int, field_name: str) -> float | None:
    values = []
    for snaps in run.snapshots_of.values():
        snap = _at(snaps, ms)
        if snap is not None:
            values.append(float(snap[field_name]))
    return round(float(np.mean(values)), 2) if values else None


# ---- derivations -----------------------------------------------------------------------------------------------------

def packs(run: Run, site_yards: float = SITE_YARDS) -> list[dict]:
    """Creature spawns grouped: a formation's members with their leader; then creatures that entered combat within
    CO_ENGAGE_MS of one another; then spawns within `site_yards` on one floor. Each pack: its spawn ids, centre,
    whether a boss is in it, how it was grouped."""
    creatures = [s for s in run.spawns.values() if s.kind == 0]
    parent = {s.spawn_id: s.spawn_id for s in creatures}
    how: dict[int, str] = {}

    def find(a: int) -> int:
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def union(a: int, b: int, reason: str) -> None:
        if a not in parent or b not in parent:
            return
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[rb] = ra
            how.setdefault(ra, reason)

    for s in creatures:
        if s.leader and s.leader in parent:
            union(s.leader, s.spawn_id, "formation")
    # Co-engagement: enter edges by time.
    if run.edges is not None and len(run.edges):
        enters = run.edges[(run.edges["event"] == r.EDGE_ENTER) & (run.edges["spawn_id"] != 0)]
        last_ms, last_sid = None, None
        for row in enters:
            sid, ms = int(row["spawn_id"]), int(row["ms"])
            if last_ms is not None and ms - last_ms <= CO_ENGAGE_MS:
                union(last_sid, sid, "co-engaged")
            last_ms, last_sid = ms, sid
    # Proximity, for what is left single.
    by_id = {s.spawn_id: s for s in creatures}
    ids = sorted(by_id)
    xy = np.array([[by_id[i].x, by_id[i].y, by_id[i].z] for i in ids]) if ids else np.zeros((0, 3))
    for a_i, a in enumerate(ids):
        if find(a) != a or how.get(a):
            continue
        d = np.hypot(xy[:, 0] - xy[a_i, 0], xy[:, 1] - xy[a_i, 1])
        close = np.flatnonzero((d <= site_yards) & (np.abs(xy[:, 2] - xy[a_i, 2]) <= SITE_RISE))
        for b_i in close:
            union(a, ids[b_i], "near")
    groups: dict[int, list[int]] = {}
    for s in creatures:
        groups.setdefault(find(s.spawn_id), []).append(s.spawn_id)
    out = []
    for root, members in groups.items():
        members.sort()
        xs = [run.spawns[m].x for m in members]
        ys = [run.spawns[m].y for m in members]
        zs = [run.spawns[m].z for m in members]
        out.append({"spawn_ids": members, "entries": sorted({run.spawns[m].entry for m in members}),
                    "centre": [round(float(np.mean(xs)), 2), round(float(np.mean(ys)), 2),
                               round(float(np.mean(zs)), 2)],
                    "boss": any(run.spawns[m].boss for m in members), "grouped_by": how.get(root, "single")})
    out.sort(key=lambda p: p["spawn_ids"][0])
    return out


def _party_enters(run: Run) -> np.ndarray:
    """Creature enter edges whose other side is the party (a human, a companion, a pet of theirs) -- or unknown
    (0: the hook had no victim), on this instance."""
    if run.edges is None or not len(run.edges):
        return np.zeros(0, r.PREFIX[r.COMBAT_EDGE][1])
    enters = run.edges[(run.edges["event"] == r.EDGE_ENTER) & (run.edges["spawn_id"] != 0)]
    if not len(enters):
        return enters
    party = np.array(sorted(run.party), dtype=np.uint64) if run.party else np.zeros(0, np.uint64)
    other = enters["other"]
    keep = (other == 0) | np.isin(other, party)
    return enters[keep]


def _puller(run: Run, row) -> tuple[int, object]:
    """The player nearest the creature at its enter edge, and their move row then."""
    best, best_row, best_d = 0, None, None
    for player in run.players:
        move = _at(run.moves_of.get(player), int(row["ms"]))
        if move is None:
            continue
        d = float(np.hypot(float(row["x"]) - float(move["x"]), float(row["y"]) - float(move["y"])))
        if best_d is None or d < best_d:
            best, best_row, best_d = player, move, d
    return best, best_row


def pulls(run: Run, pack_of: dict[int, int]) -> list[dict]:
    """The pulls in order: each starts with the first creature engaging the party while every player is out of
    combat (or PULL_GAP_MS after the last engage), and holds every creature that entered combat until the party
    left it. pull_from is where the nearest player (the puller) stood, faced and what they targeted at the first
    engage; fight_spot the median of every player's positions until the pull's last kill."""
    out: list[dict] = []
    enters = _party_enters(run)
    if not len(enters):
        return out
    current: dict | None = None
    last_enter = None
    for row in enters:
        sid, ms = int(row["spawn_id"]), int(row["ms"])
        if sid not in run.spawns:
            continue
        new = current is None or (ms - last_enter > PULL_GAP_MS and not _in_combat(run, ms - 1))
        if new:
            if current is not None:
                out.append(current)
            puller, move = _puller(run, row)
            snap = _at(run.snapshots_of.get(puller), ms) if puller else None
            current = {
                "k": len(out), "t_ms": ms - run.start_ms, "ms": ms, "first_spawn_id": sid,
                "first_entry": run.spawns[sid].entry, "spawn_ids": [], "entries": [], "packs": [],
                "puller": puller,
                "pull_from": {
                    "pos": _xyz(move) if move is not None else _xyz(row),
                    "facing": round(float(move["o"]), 3) if move is not None else None,
                    "target": int(snap["target"]) if snap is not None else 0,
                    "distance": round(float(np.hypot(float(row["x"]) - float(move["x"]),
                                                     float(row["y"]) - float(move["y"]))), 1)
                    if move is not None else None,
                },
                "creature_pos": _xyz(row), "boss": False,
            }
        if sid not in current["spawn_ids"]:
            current["spawn_ids"].append(sid)
            current["entries"].append(run.spawns[sid].entry)
            if run.spawns[sid].boss:
                current["boss"] = True
            pack = pack_of.get(sid)
            if pack is not None and pack not in current["packs"]:
                current["packs"].append(pack)
        last_enter = ms
    if current is not None:
        out.append(current)

    # Each pull's end: the first moment after its last engage with no player in combat, or the next pull's start.
    for i, pull in enumerate(out):
        start = pull["ms"]
        limit = out[i + 1]["ms"] if i + 1 < len(out) else (run.end_ms or (start + 600000))
        times = sorted({int(snap["ms"]) for snaps in run.snapshots_of.values()
                        for snap in _between(snaps, start + 500, limit)})
        end = limit
        for t in times:
            if not _in_combat(run, t):
                end = t
                break
        pull["end_ms"] = end
        pull["duration_s"] = round((end - start) / 1000.0, 1)
        kills = _between(run.kills, start, end) if run.kills is not None and len(run.kills) else []
        pull["kills"] = [int(k["victim_spawn_id"]) for k in kills if int(k["victim_spawn_id"])]
        # Creatures the threat lists put in this fight that no edge named (adds within the pull's span).
        for sid, first in run.engaged_ms.items():
            if start <= first <= end and sid not in pull["spawn_ids"]:
                pull["spawn_ids"].append(sid)
                pull["entries"].append(run.spawns[sid].entry if sid in run.spawns else 0)
                pack = pack_of.get(sid)
                if pack is not None and pack not in pull["packs"]:
                    pull["packs"].append(pack)
        last_kill = max((int(k["ms"]) for k in kills), default=end)
        pull["fight_spot"] = _median_spot(_between(run.moves, start, last_kill)) or pull["pull_from"]["pos"]
        deaths = _between(run.deaths, start, end) if run.deaths is not None and len(run.deaths) else []
        pull["deaths"] = len(deaths)

    # The pull's creatures are those that died in it (2026-10-26: the engaged-flag and co-engagement lists named
    # creatures killed elsewhere or never -- VanCleef in Greenskin's pull -- and a leg that needs them dead could not
    # be done). Every kill belongs to the last pull started at or before it: a boss can die a moment past its pull's
    # window (the party out of combat for a snapshot between its phases). What engaged without dying stays in
    # `engaged`, for reading.
    starts = [pull["ms"] for pull in out]
    died: list[list[int]] = [[] for _ in out]
    if run.kills is not None and len(run.kills) and out:
        for row in run.kills:
            sid = int(row["victim_spawn_id"])
            if not sid or sid not in run.spawns or run.spawns[sid].kind != 0:
                continue
            i = int(np.searchsorted(starts, int(row["ms"]), side="right")) - 1
            if i >= 0:
                died[i].append(sid)
    for i, pull in enumerate(out):
        pull["engaged"] = list(pull["spawn_ids"])
        pull["spawn_ids"] = list(dict.fromkeys(died[i]))
        pull["kills"] = list(pull["spawn_ids"])
        pull["entries"] = [run.spawns[sid].entry for sid in pull["spawn_ids"]]
        pull["boss"] = any(run.spawns[sid].boss for sid in pull["spawn_ids"])
        del pull["ms"]
    return out


def _still_spans(moves: np.ndarray) -> list[tuple[int, int]]:
    """Spans (ms, ms) in which one player stayed within REST_SPEED * elapsed of where the span began."""
    out: list[tuple[int, int]] = []
    if moves is None or len(moves) < 2:
        return out
    ms = moves["ms"].astype(np.int64)
    xy = np.stack([moves["x"], moves["y"]], axis=1).astype(np.float64)
    i = 0
    n = len(moves)
    while i < n - 1:
        j = i + 1
        while j < n:
            elapsed = (ms[j] - ms[i]) / 1000.0
            moved = float(np.hypot(*(xy[j] - xy[i])))
            if elapsed > 0 and moved > max(1.0, REST_SPEED * elapsed):
                break
            j += 1
        if (ms[j - 1] - ms[i]) / 1000.0 >= REST_SECONDS:
            out.append((int(ms[i]), int(ms[j - 1])))
            i = j
        else:
            i += 1
    return out


def rests(run: Run) -> list[dict]:
    """Spans of REST_SECONDS or more with every player under REST_SPEED yd/s and nobody in combat (the players'
    still spans intersected); whether an item was used in the span (food, drink, a bandage), and the party's mean
    health and power at its ends."""
    out: list[dict] = []
    per_player = [_still_spans(run.moves_of[p]) for p in run.players if len(run.moves_of.get(p, []))]
    if not per_player:
        return out
    common = per_player[0]
    for spans in per_player[1:]:
        merged = []
        for a0, a1 in common:
            for b0, b1 in spans:
                lo, hi = max(a0, b0), min(a1, b1)
                if hi - lo >= REST_SECONDS * 1000:
                    merged.append((lo, hi))
        common = merged
    for lo, hi in sorted(common):
        if _in_combat(run, lo + 1000):
            continue
        uses = _between(run.item_uses, lo, hi) if run.item_uses is not None and len(run.item_uses) else []
        first = _at(run.moves, lo)
        out.append({"t_ms": lo - run.start_ms, "seconds": round((hi - lo) / 1000.0, 1),
                    "pos": _xyz(first) if first is not None else None,
                    "items_used": sorted({int(u["item"]) for u in uses}),
                    "health_pct_from": _mean_pct(run, lo, "health_pct"), "health_pct_to": _mean_pct(run, hi, "health_pct"),
                    "power_pct_from": _mean_pct(run, lo, "power_pct"), "power_pct_to": _mean_pct(run, hi, "power_pct")})
    return out


def interactions(run: Run) -> list[dict]:
    """Any player's uses of game objects, joined to the object's state change within DOOR_JOIN_MS: Interact records
    (gossip-less uses), the client's Opening casts on an object (a door, a chest, a barrel: how a right-click
    reaches the server), and the cannon (Defias Gunpowder used, the door it opens)."""
    out = []
    uses: list[tuple[int, int, int, str, int]] = []       # (ms, spawn id, entry, how, player)
    if run.interacts is not None and len(run.interacts):
        for row in run.interacts[run.interacts["what"] == r.IA_GAME_OBJECT_USE]:
            uses.append((int(row["ms"]), int(row["spawn_id"]), int(row["entry"]), "use", int(row["player"])))
    if run.casts is not None and len(run.casts):
        for row in run.casts[(run.casts["target_kind"] == 8) & np.isin(run.casts["spell"], list(OPENING_SPELLS))]:
            uses.append((int(row["ms"]), 0, 0, "open", int(row["player"])))
    if run.item_uses is not None and len(run.item_uses):
        for row in run.item_uses[(run.item_uses["item"] == CANNON_ITEM) | (run.item_uses["spell"] == CANNON_SPELL)]:
            uses.append((int(row["ms"]), 0, 0, "cannon", int(row["player"])))
    uses.sort()
    for ms, sid, entry, how, player in uses:
        changes = _between(run.go_changes, ms - 500, ms + DOOR_JOIN_MS)
        change = None
        for c in changes:
            if (not sid and not entry) or int(c["spawn_id"]) == sid or int(c["entry"]) == entry:
                change = c
                break
        if change is None and how != "use":
            continue            # an Opening cast that changed nothing (a refused click): not an interaction
        if change is not None:
            sid = sid or int(change["spawn_id"])
            entry = entry or int(change["entry"])
        spawn = run.spawns.get(sid)
        go_type = spawn.go_type if spawn else (int(change["go_type"]) if change is not None else -1)
        move = _at(run.moves_of.get(player), ms)
        out.append({"t_ms": ms - run.start_ms, "spawn_id": sid, "entry": entry, "player": player,
                    "kind": {GO_DOOR: "door", GO_BUTTON: "button", GO_CHEST: "chest", GO_GOOBER: "goober"}.get(
                        go_type, str(go_type)),
                    "pos": [round(spawn.x, 2), round(spawn.y, 2), round(spawn.z, 2)] if spawn
                    else (_xyz(change) if change is not None else None),
                    "from": _xyz(move) if move is not None else None,
                    "changed": change is not None,
                    "go_state_after": int(change["go_state"]) if change is not None else None,
                    "change_after_ms": int(change["ms"]) - ms if change is not None else None,
                    "opened_by": how})
    # A chest is loot, not a gate -- unless the cannon is fired later: then the last chest opened before it is the
    # barrel its gunpowder came from (the Deadmines' Defias Gunpowder), a step of the route.
    cannons = [i for i in out if i["opened_by"] == "cannon"]
    for inter in out:
        inter["gate"] = inter["kind"] != "chest"
    for cannon in cannons:
        before = [i for i in out if i["kind"] == "chest" and i["t_ms"] < cannon["t_ms"]]
        if before:
            before[-1]["gate"] = True
            before[-1]["kind"] = "barrel"
    return out


def door_openings(run: Run) -> list[dict]:
    """Doors that opened on the run without a player's use -- a boss's death, a lever, a script -- each once, with
    where the party stood when it did: the gates a dungeon's route passes (the Deadmines' doors all open this way)."""
    out = []
    seen: set[int] = set()
    for row in run.go_changes:
        if int(row["go_type"]) != GO_DOOR or int(row["go_state"]) == GO_STATE_READY:
            continue
        sid = int(row["spawn_id"])
        key = sid or int(row["object"])
        if key in seen:
            continue
        seen.add(key)
        ms = int(row["ms"])
        move = _at(run.moves, ms)
        out.append({"t_ms": ms - run.start_ms, "spawn_id": sid, "entry": int(row["entry"]), "player": 0,
                    "kind": "door", "pos": _xyz(row), "from": _xyz(move) if move is not None else None,
                    "changed": True, "go_state_after": int(row["go_state"]), "change_after_ms": 0,
                    "opened_by": "script", "gate": True})
    return out


def bosses(run: Run) -> list[dict]:
    out = []
    if run.kills is None or not len(run.kills):
        return out
    for row in run.kills:
        sid = int(row["victim_spawn_id"])
        spawn = run.spawns.get(sid)
        if spawn and spawn.boss:
            out.append({"t_ms": int(row["ms"]) - run.start_ms, "spawn_id": sid, "entry": spawn.entry,
                        "pos": [round(spawn.x, 2), round(spawn.y, 2), round(spawn.z, 2)]})
    return out


def skipped(run: Run, engaged: set[int]) -> list[dict]:
    """Creatures alive at the end that a player passed within PASSED_YARDS of and the party never engaged."""
    out = []
    if run.moves is None or not len(run.moves):
        return out
    xy = np.stack([run.moves["x"], run.moves["y"]], axis=1).astype(np.float64)
    z = run.moves["z"].astype(np.float64)
    for sid, spawn in run.spawns.items():
        if spawn.kind != 0 or sid in engaged or not run.alive_at_end.get(sid, False):
            continue
        d = np.hypot(xy[:, 0] - spawn.x, xy[:, 1] - spawn.y)
        near = (d <= PASSED_YARDS) & (np.abs(z - spawn.z) <= 15.0)
        if near.any():
            k = int(np.argmax(near))
            out.append({"spawn_id": sid, "entry": spawn.entry, "pos": [round(spawn.x, 2), round(spawn.y, 2),
                                                                      round(spawn.z, 2)],
                        "closest_yards": round(float(d[near].min()), 1),
                        "t_ms": int(run.moves[k]["ms"]) - run.start_ms, "boss": spawn.boss})
    out.sort(key=lambda s: s["t_ms"])
    return out


def checkpoints(run: Run, pull_list: list[dict], interaction_list: list[dict], rest_list: list[dict]) -> list[dict]:
    """The clear way: the pulls (kind site, or boss) and the door interactions (kind gate) in time order, then the
    goal (where the path player stood last). Each site's place is where the puller pulled from; a rest that follows
    it within 60 s is its rest_after_s."""
    events: list[tuple[int, dict]] = []
    for pull in pull_list:
        events.append((pull["t_ms"], {"kind": "boss" if pull["boss"] else "site", "pos": pull["pull_from"]["pos"],
                                      "spawn_ids": list(pull["spawn_ids"]), "pull": pull["k"],
                                      "fight_spot": pull["fight_spot"], "pull_from": pull["pull_from"],
                                      "end_t_ms": pull["end_ms"] - run.start_ms}))
    gates_seen: set[int] = set()
    for inter in interaction_list:
        if inter.get("gate") and inter["changed"] and inter["pos"] and inter["spawn_id"]:
            if inter["spawn_id"] in gates_seen:
                continue
            gates_seen.add(inter["spawn_id"])
            events.append((inter["t_ms"], {"kind": "gate", "pos": inter["from"] or inter["pos"],
                                           "spawn_ids": [inter["spawn_id"]] if inter["spawn_id"] else [],
                                           "object_pos": inter["pos"], "entry": inter["entry"],
                                           "opened_by": inter.get("opened_by", "use")}))
    events.sort(key=lambda e: e[0])
    out = []
    for t_ms, event in events:
        rest_after = 0.0
        end = event.pop("end_t_ms", t_ms)
        for rest in rest_list:
            if end <= rest["t_ms"] <= end + 60000:
                rest_after = rest["seconds"]
                break
        x, y, z = event.pop("pos")
        out.append({"k": len(out), "kind": event.pop("kind"), "x": x, "y": y, "z": z, "t_ms": t_ms,
                    "rest_after_s": rest_after, **event})
    line = run.moves_of.get(run.path_player)
    if line is not None and len(line):
        last = line[-1]
        x, y, z = _xyz(last)
        out.append({"k": len(out), "kind": "goal", "x": x, "y": y, "z": z, "t_ms": int(last["ms"]) - run.start_ms,
                    "rest_after_s": 0.0, "spawn_ids": []})
    return out


# ---- the report ------------------------------------------------------------------------------------------------------

def analyse(cap: r.CaptureDir, span: Span, site_yards: float = SITE_YARDS) -> dict:
    run = load(cap, span)
    pack_list = packs(run, site_yards)
    pack_of = {sid: i for i, pack in enumerate(pack_list) for sid in pack["spawn_ids"]}
    pull_list = pulls(run, pack_of)
    rest_list = rests(run)
    interaction_list = interactions(run) + door_openings(run)
    interaction_list.sort(key=lambda i: i["t_ms"])
    boss_list = bosses(run)
    engaged = {sid for pull in pull_list for sid in pull["spawn_ids"]}
    skipped_list = skipped(run, engaged)
    death_list = [{"t_ms": int(d["ms"]) - run.start_ms, "player": int(d["player"]), "pos": _xyz(d),
                   "cause": int(d["cause"]), "killer": int(d["killer"])}
                  for d in (run.deaths if run.deaths is not None else [])]
    duration_ms = (run.end_ms - run.start_ms) if run.end_ms else 0
    # One player's trajectory leg by leg (the one with the most movement packets; leg k: from checkpoint k-1's time,
    # the start for leg 0, to checkpoint k's), their moves on the map at most PATH_STEP_MS apart: what the sim
    # measures each seat's line against.
    cps = checkpoints(run, pull_list, interaction_list, rest_list)
    path_rows: list[list] = []
    line = run.moves_of.get(run.path_player)
    if line is not None and len(line):
        ms = line["ms"].astype(np.int64) - run.start_ms
        bounds = [0] + [int(cp["t_ms"]) for cp in cps]
        for k in range(len(cps)):
            lo, hi = bounds[k], bounds[k + 1]
            idx = np.flatnonzero((ms >= lo) & (ms <= hi))
            # The last move at or before the leg's start too, so a leg's line begins where the player stood.
            before = np.flatnonzero(ms < lo)
            if len(before):
                idx = np.concatenate([[before[-1]], idx])
            last_t = None
            for i in idx:
                t = int(ms[i])
                if last_t is not None and t - last_t < PATH_STEP_MS and i != idx[-1]:
                    continue
                last_t = t
                row = line[i]
                path_rows.append([k, max(t, lo), round(float(row["x"]), 2), round(float(row["y"]), 2),
                                  round(float(row["z"]), 2)])
            if len(idx) == 0 or path_rows[-1][0] != k or path_rows[-1][1] < hi:
                # The leg's end at its checkpoint and time (a leg with no moves: a stand-still pull).
                path_rows.append([k, hi, cps[k]["x"], cps[k]["y"], cps[k]["z"]])
    return {
        "format": 2, "source": {"capture": str(cap.root), "map": span.map, "instance": span.instance,
                                "start_ms": span.start_ms, "end_ms": span.end_ms, "span": span.index},
        "players": list(run.players), "companions": list(span.companions), "path_player": run.path_player,
        "map": run.map, "instance": run.instance, "duration_s": round(duration_ms / 1000.0, 1),
        "entrance": [round(v, 2) for v in run.entrance] if run.entrance else None,
        "checkpoints": cps,
        "path": path_rows,
        "packs": pack_list, "pulls": pull_list, "rests": rest_list, "interactions": interaction_list,
        "bosses": boss_list, "skipped": skipped_list, "deaths": death_list,
        "counts": {"spawns": len(run.spawns), "creatures": sum(1 for s in run.spawns.values() if s.kind == 0),
                   "packs": len(pack_list), "pulls": len(pull_list), "engaged": len(engaged),
                   "rests": len(rest_list), "interactions": len(interaction_list), "bosses": len(boss_list),
                   "skipped": len(skipped_list), "deaths": len(death_list),
                   "moves": int(len(run.moves)) if run.moves is not None else 0},
    }


def write(report: dict, out: str | Path) -> dict[str, str]:
    """The report as JSON and the clear way as `<out>.clearway.csv` (what the core's ClearWayFile loads:
    `index,kind,x,y,z,spawn_ids`, spawn ids `;`-joined), the path player's line as `<out>.path.csv`."""
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=1))
    way = out.with_suffix(".clearway.csv") if out.suffix == ".json" else Path(str(out) + ".clearway.csv")
    with way.open("w", newline="") as fh:
        writer = csv.writer(fh, lineterminator="\n")      # never CRLF: a CR on the last field loses a spawn id
        writer.writerow(["index", "kind", "x", "y", "z", "spawn_ids"])
        for cp in report["checkpoints"]:
            writer.writerow([cp["k"], cp["kind"], cp["x"], cp["y"], cp["z"],
                             ";".join(str(s) for s in cp["spawn_ids"])])
    path = way.with_name(way.name.replace(".clearway.csv", ".path.csv"))
    with path.open("w", newline="") as fh:
        writer = csv.writer(fh, lineterminator="\n")
        writer.writerow(["leg", "t_ms", "x", "y", "z"])
        for row in report.get("path", []):
            writer.writerow(row)
    return {"report": str(out), "clearway": str(way), "path": str(path)}
