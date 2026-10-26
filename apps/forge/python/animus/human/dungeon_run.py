"""A dungeon run read off a recording (FORMAT.md §1): the packs, the pulls in the order the player made them, the
rests, the door and lever interactions, the bosses, what was skipped, the deaths, and the notes -- and from those
the **clear way** the forge's PointNav stages follow (`AnimusForge.Curriculum.PointNav.ClearWayFile`), so the bots
learn the dungeon as a person runs it rather than a generated route between spawn clusters.

    python -m animus.human dungeon-run <recording dir | --capture <root> --recording <id>> --out <report.json>

Nothing here is typed by the player: pulls come from CombatEdge (a creature entering combat with the group),
packs from the map's formations, then co-engagement, then spawn proximity; rests from standing still out of
combat; interactions from Interact joined to the object's state change; bosses from the spawn table's boss flag.
The notes are carried through as they are. Beside the JSON, `<out>.clearway.csv` (`index,kind,x,y,z,spawn_ids`) is
what the core loads.
"""

from __future__ import annotations

import csv
import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from animus.human import reader as r
from animus.human import recordings

SITE_YARDS = 30.0           # spawns this close (and on one floor) are one pack when nothing else says
SITE_RISE = 6.0
CO_ENGAGE_MS = 2000         # creatures entering combat within this of one another are one pull
PULL_GAP_MS = 3000          # a creature engaging this long after the pull's last, with the player out of combat,
                            # starts a new pull
PATH_STEP_MS = 500          # the recorded line's samples at most this often (a leg's path for the sim)
REST_SPEED = 0.3            # yd/s: standing still
REST_SECONDS = 5.0
DOOR_JOIN_MS = 3000         # an Interact and the object's state change this close are one interaction
PASSED_YARDS = 40.0         # a creature alive at the end that the player came this close to was skipped
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
    rec: r.RecordingDir
    player: int = 0
    map: int = 0
    instance: int = 0
    start_ms: int = 0
    end_ms: int = 0
    entrance: tuple[float, float, float] | None = None
    spawns: dict[int, Spawn] = field(default_factory=dict)
    moves: np.ndarray | None = None             # the player's Move rows on the map, by ms
    snapshots: np.ndarray | None = None         # the player's Snapshot rows, by ms
    edges: np.ndarray | None = None             # CombatEdge rows on the map
    kills: np.ndarray | None = None
    deaths: np.ndarray | None = None
    interacts: np.ndarray | None = None
    go_changes: np.ndarray | None = None        # GameObjectState rows with event changed
    item_uses: np.ndarray | None = None
    casts: np.ndarray | None = None
    alive_at_end: dict[int, bool] = field(default_factory=dict)
    threat_members: dict[int, set[int]] = field(default_factory=dict)   # creature spawn -> units ever on its list
    engaged_ms: dict[int, int] = field(default_factory=dict)            # creature spawn -> first UnitState engaged


# ---- loading ---------------------------------------------------------------------------------------------------------

def _sorted(rows: np.ndarray) -> np.ndarray:
    return rows[np.argsort(rows["ms"], kind="stable")] if len(rows) else rows


def load(rec: r.RecordingDir, map_id: int | None = None) -> Run:
    """Everything the derivations need, from the recording's streams. The run is the recording's dungeon map: the
    one it entered last (or `map_id`); its world-map walk to the entrance is left out."""
    run = Run(rec)
    start, _ = recordings.start_record(rec)
    if start is not None:
        run.player = int(start["player"])
        run.start_ms = int(start["ms"])
    end, _ = recordings.end_record(rec)
    run.end_ms = int(end["ms"]) if end is not None else 0

    session = rec.load("session")
    if not run.player:
        starts = session.get(r.SESSION_START)
        if len(starts):
            run.player = int(starts[0]["player"])

    # The dungeon: the last MapSpawns map (a recording writes one per map it enters), unless told. MapSpawns and
    # InstanceCreatures are snapshot-stream records (types 34 and 35).
    maps_seen: list[int] = []
    spawn_parts: dict[int, list[np.ndarray]] = {}
    alive_parts: dict[tuple[int, int], dict[int, np.ndarray]] = {}
    for batch in rec.read("snapshot"):
        rows = batch.get(r.MAP_SPAWNS)
        for i, row in enumerate(rows):
            m = int(row["map"])
            if m not in maps_seen:
                maps_seen.append(m)
            spawn_parts.setdefault(m, []).append(batch.tail_rows(r.MAP_SPAWNS, i))
        rows = batch.get(r.INSTANCE_CREATURES)
        for i, row in enumerate(rows):
            key = (int(row["map"]), int(row["instance"]))
            # The last dump wins per (map, instance); its parts are keyed by part index.
            dump = alive_parts.setdefault(key, {})
            if int(row["part"]) == 0:
                dump.clear()
            dump[int(row["part"])] = batch.tail_rows(r.INSTANCE_CREATURES, i)
    if map_id is None:
        if maps_seen:
            map_id = maps_seen[-1]
        elif start is not None:
            map_id = int(start["map"])
        else:
            map_id = 0
    run.map = map_id
    for part in spawn_parts.get(map_id, []):
        for row in part:
            run.spawns[int(row["spawn_id"])] = Spawn(int(row["spawn_id"]), int(row["entry"]), float(row["x"]),
                                                     float(row["y"]), float(row["z"]), int(row["rank"]),
                                                     bool(row["boss"]), int(row["formation_leader_spawn"]),
                                                     int(row["kind"]), int(row["go_type"]))

    moves = rec.load("move").get(r.MOVE)
    moves = moves[(moves["player"] == run.player) & (moves["map"] == map_id)] if len(moves) else moves
    run.moves = _sorted(moves)
    if len(run.moves):
        first = run.moves[0]
        run.entrance = (float(first["x"]), float(first["y"]), float(first["z"]))
    if start is not None and int(start["map"]) == map_id:
        run.entrance = (float(start["x"]), float(start["y"]), float(start["z"]))

    # Snapshots and unit states: the player's own state by time; creature threat lists and engaged flags.
    snaps = []
    for batch in rec.read("snapshot"):
        rows = batch.get(r.SNAPSHOT)
        if len(rows):
            snaps.append(rows[(rows["player"] == run.player) & (rows["map"] == map_id)])
        units = batch.get(r.UNIT_STATE)
        if len(units):
            mine = np.flatnonzero((units["player"] == run.player) & (units["unit_type"] == 0)
                                  & (units["spawn_id"] != 0))
            for i in mine:
                row = units[i]
                sid = int(row["spawn_id"])
                if sid not in run.spawns:
                    continue
                if int(row["flags"]) & r.US_ENGAGED and sid not in run.engaged_ms:
                    run.engaged_ms[sid] = int(row["ms"])
                if int(row["flags"]) & r.US_COMBAT:
                    threats = batch.unit_state_details(int(i))["threats"]
                    if len(threats):
                        run.threat_members.setdefault(sid, set()).update(int(u) for u in threats["unit"])
        changes = batch.get(r.GAME_OBJECT_STATE)
        if len(changes):
            changes = changes[(changes["player"] == run.player) & (changes["event"] == r.GO_CHANGED)]
            run.go_changes = changes if run.go_changes is None else np.concatenate([run.go_changes, changes])
    run.snapshots = _sorted(np.concatenate(snaps)) if snaps else np.zeros(0, r.PREFIX[r.SNAPSHOT][1])
    run.go_changes = _sorted(run.go_changes) if run.go_changes is not None \
        else np.zeros(0, r.PREFIX[r.GAME_OBJECT_STATE][1])

    outcome = rec.load("outcome")
    edges = outcome.get(r.COMBAT_EDGE)
    if len(edges):
        edges = edges[edges["map"] == map_id]
        instances = np.unique(edges["instance"])
        run.instance = int(instances[-1]) if len(instances) else 0
    run.edges = _sorted(edges)
    run.kills = _sorted(outcome.get(r.KILL))
    deaths = outcome.get(r.DEATH)
    run.deaths = _sorted(deaths[(deaths["player"] == run.player) & (deaths["map"] == map_id)]) if len(deaths) \
        else deaths

    action = rec.load("action")
    interacts = action.get(r.INTERACT)
    run.interacts = _sorted(interacts[interacts["player"] == run.player]) if len(interacts) else interacts
    uses = action.get(r.ITEM_USE)
    run.item_uses = _sorted(uses[uses["player"] == run.player]) if len(uses) else uses
    casts = action.get(r.CAST_REQUEST)
    run.casts = _sorted(casts[casts["player"] == run.player]) if len(casts) else casts

    for (m, inst), parts in alive_parts.items():
        if m != map_id or (run.instance and inst != run.instance):
            continue
        for part in parts.values():
            for row in part:
                run.alive_at_end[int(row["spawn_id"])] = bool(row["alive"])
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
    snap = _at(run.snapshots, ms)
    return bool(snap is not None and int(snap["flags"]) & r.SELF_COMBAT)


def _xyz(row) -> list[float]:
    return [round(float(row["x"]), 2), round(float(row["y"]), 2), round(float(row["z"]), 2)]


def _median_spot(moves: np.ndarray) -> list[float] | None:
    if moves is None or not len(moves):
        return None
    return [round(float(np.median(moves[c])), 2) for c in ("x", "y", "z")]


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


def pulls(run: Run, pack_of: dict[int, int]) -> list[dict]:
    """The pulls in order: each starts with the first creature engaging while the player is out of combat (or
    PULL_GAP_MS after the last engage), and holds every creature that entered combat until the player left it.
    pull_from is where the player stood, faced and what they targeted at the first engage; fight_spot the median
    of their positions until the pull's last kill."""
    out: list[dict] = []
    if run.edges is None or not len(run.edges):
        return out
    enters = run.edges[(run.edges["event"] == r.EDGE_ENTER) & (run.edges["spawn_id"] != 0)]
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
            move = _at(run.moves, ms)
            snap = _at(run.snapshots, ms)
            current = {
                "k": len(out), "t_ms": ms - run.start_ms, "ms": ms, "first_spawn_id": sid,
                "first_entry": run.spawns[sid].entry, "spawn_ids": [], "entries": [], "packs": [],
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

    # Each pull's end: the player's first out-of-combat snapshot after its last engage, or the next pull's start.
    for i, pull in enumerate(out):
        start = pull["ms"]
        limit = out[i + 1]["ms"] if i + 1 < len(out) else (run.end_ms or (start + 600000))
        snaps = _between(run.snapshots, start + 500, limit)
        end = limit
        for snap in snaps:
            if not int(snap["flags"]) & r.SELF_COMBAT:
                end = int(snap["ms"])
                break
        pull["end_ms"] = end
        pull["duration_s"] = round((end - start) / 1000.0, 1)
        kills = _between(run.kills, start, end) if run.kills is not None and len(run.kills) else []
        pull["kills"] = [int(k["victim_spawn_id"]) for k in kills if int(k["victim_spawn_id"])]
        # Creatures the threat lists put in this fight that no edge named (adds within the pull's span).
        for sid, first in run.engaged_ms.items():
            if start <= first <= end and sid not in pull["spawn_ids"]:
                pull["spawn_ids"].append(sid)
                pull["entries"].append(run.spawns[sid].entry)
                pack = pack_of.get(sid)
                if pack is not None and pack not in pull["packs"]:
                    pull["packs"].append(pack)
        last_kill = max((int(k["ms"]) for k in kills), default=end)
        pull["fight_spot"] = _median_spot(_between(run.moves, start, last_kill)) or pull["pull_from"]["pos"]
        deaths = _between(run.deaths, start, end) if run.deaths is not None and len(run.deaths) else []
        pull["deaths"] = len(deaths)
        del pull["ms"]
    return out


def rests(run: Run) -> list[dict]:
    """Spans of REST_SECONDS or more with the player under REST_SPEED yd/s and out of combat; whether an item was
    used in the span (food, drink, a bandage)."""
    out: list[dict] = []
    moves = run.moves
    if moves is None or len(moves) < 2:
        return out
    ms = moves["ms"].astype(np.int64)
    xy = np.stack([moves["x"], moves["y"]], axis=1).astype(np.float64)
    i = 0
    n = len(moves)
    while i < n - 1:
        j = i + 1
        # Grow while the player stays within REST_SPEED * elapsed of where the span began.
        while j < n:
            elapsed = (ms[j] - ms[i]) / 1000.0
            moved = float(np.hypot(*(xy[j] - xy[i])))
            if elapsed > 0 and moved > max(1.0, REST_SPEED * elapsed):
                break
            j += 1
        span = (ms[j - 1] - ms[i]) / 1000.0
        if span >= REST_SECONDS and not _in_combat(run, int(ms[i]) + 1000):
            uses = _between(run.item_uses, int(ms[i]), int(ms[j - 1])) if run.item_uses is not None \
                and len(run.item_uses) else []
            snap_start, snap_end = _at(run.snapshots, int(ms[i])), _at(run.snapshots, int(ms[j - 1]))
            out.append({"t_ms": int(ms[i]) - run.start_ms, "seconds": round(span, 1), "pos": _xyz(moves[i]),
                        "items_used": sorted({int(u["item"]) for u in uses}),
                        "health_pct_from": round(float(snap_start["health_pct"]), 2) if snap_start is not None
                        else None,
                        "health_pct_to": round(float(snap_end["health_pct"]), 2) if snap_end is not None else None,
                        "power_pct_from": round(float(snap_start["power_pct"]), 2) if snap_start is not None
                        else None,
                        "power_pct_to": round(float(snap_end["power_pct"]), 2) if snap_end is not None else None})
            i = j
        else:
            i += 1
    return out


def interactions(run: Run) -> list[dict]:
    """The player's uses of game objects, joined to the object's state change within DOOR_JOIN_MS: Interact records
    (gossip-less uses), the client's Opening casts on an object (a door, a chest, a barrel: how a right-click
    reaches the server), and the cannon (Defias Gunpowder used, the door it opens)."""
    out = []
    uses: list[tuple[int, int, int, str]] = []       # (ms, spawn id, entry, how)
    if run.interacts is not None and len(run.interacts):
        for row in run.interacts[run.interacts["what"] == r.IA_GAME_OBJECT_USE]:
            uses.append((int(row["ms"]), int(row["spawn_id"]), int(row["entry"]), "use"))
    if run.casts is not None and len(run.casts):
        for row in run.casts[(run.casts["target_kind"] == 8) & np.isin(run.casts["spell"], list(OPENING_SPELLS))]:
            uses.append((int(row["ms"]), 0, 0, "open"))
    if run.item_uses is not None and len(run.item_uses):
        for row in run.item_uses[(run.item_uses["item"] == CANNON_ITEM) | (run.item_uses["spell"] == CANNON_SPELL)]:
            uses.append((int(row["ms"]), 0, 0, "cannon"))
    uses.sort()
    for ms, sid, entry, how in uses:
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
        move = _at(run.moves, ms)
        out.append({"t_ms": ms - run.start_ms, "spawn_id": sid, "entry": entry,
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
    where the player stood when it did: the gates a dungeon's route passes (the Deadmines' doors all open this way)."""
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
        out.append({"t_ms": ms - run.start_ms, "spawn_id": sid, "entry": int(row["entry"]), "kind": "door",
                    "pos": _xyz(row), "from": _xyz(move) if move is not None else None, "changed": True,
                    "go_state_after": int(row["go_state"]), "change_after_ms": 0, "opened_by": "script",
                    "gate": True})
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
    """Creatures alive at the end that the player passed within PASSED_YARDS of and never engaged."""
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
    goal (where the player stood last). Each site's place is where the player pulled from; a rest that follows it
    within 60 s is its rest_after_s."""
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
    if run.moves is not None and len(run.moves):
        last = run.moves[-1]
        x, y, z = _xyz(last)
        out.append({"k": len(out), "kind": "goal", "x": x, "y": y, "z": z, "t_ms": int(last["ms"]) - run.start_ms,
                    "rest_after_s": 0.0, "spawn_ids": []})
    return out


# ---- the report ------------------------------------------------------------------------------------------------------

def analyse(rec: r.RecordingDir, site_yards: float = SITE_YARDS, map_id: int | None = None) -> dict:
    run = load(rec, map_id)
    pack_list = packs(run, site_yards)
    pack_of = {sid: i for i, pack in enumerate(pack_list) for sid in pack["spawn_ids"]}
    pull_list = pulls(run, pack_of)
    rest_list = rests(run)
    interaction_list = interactions(run) + door_openings(run)
    interaction_list.sort(key=lambda i: i["t_ms"])
    boss_list = bosses(run)
    engaged = {sid for pull in pull_list for sid in pull["spawn_ids"]}
    skipped_list = skipped(run, engaged)
    death_list = [{"t_ms": int(d["ms"]) - run.start_ms, "pos": _xyz(d), "cause": int(d["cause"]),
                   "killer": int(d["killer"])} for d in (run.deaths if run.deaths is not None else [])]
    note_list = [{k: v for k, v in n.items() if k not in ("move_ms", "move_x", "move_y", "move_z")}
                 for n in recordings.notes(rec)]
    for note in note_list:
        note["t_ms"] = note["ms"] - run.start_ms
    manifest = rec.manifest or {}
    duration_ms = (run.end_ms - run.start_ms) if run.end_ms else (int(run.moves[-1]["ms"]) - run.start_ms
                                                                   if run.moves is not None and len(run.moves) else 0)
    # The recorded trajectory leg by leg (leg k: from checkpoint k-1's time, the start for leg 0, to checkpoint k's),
    # the player's moves on the map at most PATH_STEP_MS apart: what the sim measures each seat's line against.
    cps = checkpoints(run, pull_list, interaction_list, rest_list)
    path_rows: list[list] = []
    if run.moves is not None and len(run.moves):
        ms = run.moves["ms"].astype(np.int64) - run.start_ms
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
                row = run.moves[i]
                path_rows.append([k, max(t, lo), round(float(row["x"]), 2), round(float(row["y"]), 2),
                                  round(float(row["z"]), 2)])
            if len(idx) == 0 or path_rows[-1][0] != k or path_rows[-1][1] < hi:
                # The leg's end at its checkpoint and time (a leg with no moves: a stand-still pull).
                path_rows.append([k, hi, cps[k]["x"], cps[k]["y"], cps[k]["z"]])
    return {
        "format": 1, "source": rec.id, "title": rec.title, "verdict": rec.verdict,
        "character": manifest.get("character"), "flags": manifest.get("flags"),
        "map": run.map, "instance": run.instance, "duration_s": round(duration_ms / 1000.0, 1),
        "entrance": [round(v, 2) for v in run.entrance] if run.entrance else None,
        "checkpoints": cps,
        "path": path_rows,
        "packs": pack_list, "pulls": pull_list, "rests": rest_list, "interactions": interaction_list,
        "bosses": boss_list, "skipped": skipped_list, "deaths": death_list, "notes": note_list,
        "counts": {"spawns": len(run.spawns), "creatures": sum(1 for s in run.spawns.values() if s.kind == 0),
                   "packs": len(pack_list), "pulls": len(pull_list), "engaged": len(engaged),
                   "rests": len(rest_list), "interactions": len(interaction_list), "bosses": len(boss_list),
                   "skipped": len(skipped_list), "deaths": len(death_list), "notes": len(note_list),
                   "moves": int(len(run.moves)) if run.moves is not None else 0},
    }


def write(report: dict, out: str | Path) -> dict[str, str]:
    """The report as JSON and the clear way as `<out>.clearway.csv` (what the core's ClearWayFile loads:
    `index,kind,x,y,z,spawn_ids`, spawn ids `;`-joined)."""
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
