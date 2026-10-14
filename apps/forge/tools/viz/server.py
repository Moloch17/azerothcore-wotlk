#!/usr/bin/env python3
"""Forge route viewer: a small web server for looking at the bots' evaluation routes on any baked map.

    python3 apps/forge/tools/viz/server.py [--runs DIR] [--scenes DIR] [--host 0.0.0.0] [--port 8770]

then open http://<this machine>:8770/ in a browser.

It reads a run folder as the learner writes it -- eval_trace_<env_steps>_<policy>.npz (every decision of every seat:
position, heading, camera, keys, wall contact), eval_episodes.jsonl (one row per episode: map, class, outcome, object
and end position) and stage.json (the room and object names) -- and draws each map's background from its baked camera
scene (<scenes>/<map id, 3 digits>.scene), so any map that has a scene works without a hand-drawn layout. Everything
it makes is kept in the run's own folder, runs/<run>/viz/ (the rendered backgrounds), never elsewhere.

Read-only towards training: it opens the run's files, never writes to anything but runs/<run>/viz/.
"""
import argparse
import gzip
import io
import json
import math
import re
import struct
import threading
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parents[4]
STATIC = Path(__file__).resolve().parent / "static"
TRACE_RE = re.compile(r"^eval_trace_(\d+)_(.+)\.npz$")
MAP_NAMES = {34: "The Stockade", 36: "The Deadmines", 389: "Ragefire Chasm", 43: "Wailing Caverns"}
RACES = {1: "human", 2: "orc", 3: "dwarf", 4: "night elf", 5: "undead", 6: "tauren", 7: "gnome", 8: "troll",
         10: "blood elf", 11: "draenei"}
PX = 0.5                     # yards per background pixel
FLOOR_NZ = 0.65              # a face whose normal points this much up is floor
MARGIN = 14.0                # yards around the routes' extent

_lock = threading.Lock()
_cache = {}                  # (kind, key...) -> (mtime, value)


# ---- run folders ----------------------------------------------------------------------------------------------------

def runs_index(runs_dir):
    out = []
    for d in sorted((p for p in runs_dir.iterdir() if p.is_dir()), key=lambda p: p.stat().st_mtime, reverse=True):
        evals = {}
        for f in d.iterdir():
            m = TRACE_RE.match(f.name)
            if m:
                evals.setdefault(int(m.group(1)), []).append(m.group(2))
        if not evals:
            continue
        out.append({
            "name": d.name,
            "evals": [{"steps": s, "policies": sorted(p, key=lambda x: (x != "learner", x))}
                      for s, p in sorted(evals.items())],
        })
    return out


def _cached(kind, path, build):
    key = (kind, str(path))
    mtime = path.stat().st_mtime
    with _lock:
        hit = _cache.get(key)
        if hit and hit[0] == mtime:
            return hit[1]
    value = build()
    with _lock:
        _cache[key] = (mtime, value)
    return value


def _categories(run):
    def build():
        try:
            return json.loads((run / "stage.json").read_text()).get("episode_categories", {})
        except (OSError, ValueError):
            return {}
    stage = run / "stage.json"
    return _cached("categories", stage, build) if stage.exists() else {}


def _num(v, default=None):
    try:
        f = float(v)
        return default if math.isnan(f) else f
    except (TypeError, ValueError):
        return default


_STEPS_RE = re.compile(rb'"env_steps":\s*(\d+)')
_POLICY_RE = re.compile(rb'"policy":\s*"([^"]*)"')
_index_lock = threading.Lock()
_index = {}                  # path -> {"ino", "size", "spans": {(steps, policy): [(offset, length)]}}
_rows_cache = {}             # (path, steps, policy, size) -> parsed rows; the few most recent evaluations only
ROWS_CACHE = 6


def _episode_index(path):
    """Where each evaluation's lines are in eval_episodes.jsonl: (env_steps, policy) -> [(offset, length)]. Only the
    bytes added since the last look are scanned, and only for those two fields (the file holds every evaluation the
    run ever played: parsing it whole on each new evaluation was the slow load)."""
    stat = path.stat()
    with _index_lock:
        entry = _index.get(str(path))
        if not entry or entry["ino"] != stat.st_ino or stat.st_size < entry["size"]:
            entry = {"ino": stat.st_ino, "size": 0, "spans": {}}
            _index[str(path)] = entry
        if stat.st_size > entry["size"]:
            with open(path, "rb") as f:
                f.seek(entry["size"])
                offset = entry["size"]
                for line in f:
                    if not line.endswith(b"\n"):
                        break                       # a line still being written: next time
                    steps, policy = _STEPS_RE.search(line), _POLICY_RE.search(line)
                    if steps and policy:
                        key = (int(steps.group(1)), policy.group(1).decode())
                        entry["spans"].setdefault(key, []).append((offset, len(line)))
                    offset += len(line)
                entry["size"] = offset
        return entry


def _episode_rows(run, steps, policy):
    """The selected evaluation's rows of eval_episodes.jsonl, and nothing else read or parsed."""
    path = run / "eval_episodes.jsonl"
    if not path.exists():
        return []
    entry = _episode_index(path)
    key = (str(path), steps, policy, entry["size"])
    with _index_lock:
        rows = _rows_cache.get(key)
        spans = list(entry["spans"].get((steps, policy), ()))
    if rows is not None:
        return rows
    rows = []
    with open(path, "rb") as f:
        for offset, length in spans:
            f.seek(offset)
            try:
                rows.append(json.loads(f.read(length)))
            except ValueError:
                continue
    with _index_lock:
        _rows_cache[key] = rows
        while len(_rows_cache) > ROWS_CACHE:
            _rows_cache.pop(next(iter(_rows_cache)))
    return rows


#: A follow episode (move4_follow, decision 0033) counts as kept, the viewer's "found", at this share of its time kept
#: within the band of the leader (the stage's ladder gate).
FOLLOW_KEPT = 0.75


def _is_nav(r):
    """A nav stage's episode (decision 0036: PointNavEncounter): a point goal, its route known to the sim."""
    return r.get("nav_rung") is not None and r.get("goal_x") is not None


def _map_of(r):
    """The episode's map: the seek stage's seek_map, the follow stage's follow_map, a nav stage's map."""
    if _is_nav(r) and r.get("follow_map") is None:
        return int(_num(r.get("map"), 0))
    return int(_num(r.get("seek_map"), _num(r.get("follow_map"), 0)))


def map_points(run, steps, policy, map_id):
    """The selected evaluation's object (a follow episode's start) and end positions on the map: with its routes, the
    searched area."""
    pts = []
    for r in _episode_rows(run, steps, policy):
        if _map_of(r) != map_id:
            continue
        for p in ("object", "end", "start", "goal"):
            xyz = [_num(r.get(f"{p}_{a}")) for a in "xyz"]
            if None not in xyz:
                pts.append(xyz)
    a = np.array(pts, dtype=np.float64).reshape(-1, 3)
    return a[:, 0], a[:, 1], a[:, 2]


def episodes(run, steps, policy):
    """The evaluation's episodes, keyed by (seed, agent), with the fields the viewer shows."""
    rows = _episode_rows(run, steps, policy)
    cats = _categories(run)
    rooms, objects = cats.get("seek_room", []), cats.get("seek_object", [])
    out = []
    for r in rows:
        room_i, obj_i = _num(r.get("seek_room")), _num(r.get("seek_object"))
        kept = _num(r.get("follow_kept_share"))
        follow = r.get("follow_map") is not None and r.get("seek_map") is None
        nav = _is_nav(r) and not follow
        summary = None
        if nav:
            # A nav episode: "found" is arrived, its object the goal point, and its line the route numbers.
            spl, optimal = _num(r.get("spl")), _num(r.get("optimal_yards"))
            detour, travelled = _num(r.get("detour_factor")), _num(r.get("travelled_yards"))
            parts = [f"spl {spl:.2f}" if spl is not None else "spl -"]
            if optimal is not None:
                parts.append(f"route {round(optimal)} yd" + (f" x{detour:.2f}" if detour else ""))
            if travelled is not None:
                parts.append(f"walked {round(travelled)} yd")
            stop = _num(r.get("stop_error_yards"))
            if _num(r.get("arrived"), 0) and stop is not None:
                parts.append(f"stopped {stop:.1f} yd off")
            if _num(r.get("died"), 0):
                parts.append("died")
            summary = " · ".join(parts)
        elif follow:
            # A follow episode: "found" is kept with the leader, its object the start, and its line the follow numbers.
            parts = [f"kept {round((kept or 0) * 100)}%"]
            for key, label in (("settled_share", "settled"), ("regroup_share", "regrouped")):
                value = _num(r.get(key))
                if value is not None:
                    parts.append(f"{label} {round(value * 100)}%")
            lost = _num(r.get("lost_seconds"))
            if lost:
                parts.append(f"lost {round(lost)} s")
            summary = " · ".join(parts)
        out.append({
            "seed": int(r.get("seed", -1)), "agent": int(r.get("agent", 0) or 0),
            "map": _map_of(r),
            "found": (bool(_num(r.get("arrived"), 0)) if nav else (kept or 0) >= FOLLOW_KEPT if follow
                      else bool(_num(r.get("found"), 0))),
            "timed_out": bool(_num(r.get("timed_out"), 0)),
            "find_seconds": _num(r.get("find_seconds")),
            "summary": summary,
            "class": r.get("layout") or r.get("class") or "?", "race": RACES.get(int(_num(r.get("race"), 0)), r.get("race")), "level": _num(r.get("level")),
            "room": rooms[int(room_i)] if room_i is not None and 0 <= int(room_i) < len(rooms) else None,
            "object": objects[int(obj_i)] if obj_i is not None and 0 <= int(obj_i) < len(objects) else None,
            "object_xyz": ([_num(r.get("goal_x")), _num(r.get("goal_y")), _num(r.get("goal_z"))] if nav else
                           [_num(r.get("start_x")), _num(r.get("start_y")), _num(r.get("start_z"))] if follow else
                           [_num(r.get("object_x")), _num(r.get("object_y")), _num(r.get("object_z"))]),
            "end_xyz": [_num(r.get("end_x")), _num(r.get("end_y")), _num(r.get("end_z"))],
            "pin_events": _num(r.get("pin_events")), "wall_seconds": _num(r.get("wall_seconds")),
            "return": _num(r.get("return")), "rung": _num(r.get("nav_rung") if nav else r.get("seek_rung")),
            # Systematic search (decision 0030): the rung's rooms covered, the seconds to half of them, wasted travel.
            "checked_share": _num(r.get("checked_share")), "half_checked_s": _num(r.get("half_checked_s")),
            "searched_revisit_s": _num(r.get("searched_revisit_s")),
        })
    return out


def trace(run, steps, policy):
    """Every scored seat's decisions: one block per episode."""
    path = run / f"eval_trace_{steps}_{policy}.npz"

    def build():
        z = np.load(path, allow_pickle=False)
        starts = z["starts"]
        cols = {k: z[k] for k in z.files if k not in ("starts", "meta")}
        meta = json.loads(str(z["meta"])) if "meta" in z.files else {}
        return starts, cols, meta
    return _cached("trace", path, build)


def _searched_to_cells(run):
    """How the map block's searched_cells scalar maps back to 1-yd cells, from the run's stage.json: log1p over
    searched_cells_log_max (map block revision 4, decision 0031), else linear over floor_scale (6000 before)."""
    def build():
        try:
            layouts = json.loads((run / "stage.json").read_text()).get("layouts") or {}
        except (OSError, ValueError):
            layouts = {}
        for entry in layouts.values():
            for block in entry.get("blocks", ()):
                spec = block.get("map") if block.get("name") == "map" else None
                if spec and spec.get("searched_cells_log_max"):
                    top = math.log1p(float(spec["searched_cells_log_max"]))
                    return lambda v: math.expm1(v * top)
                if spec and spec.get("floor_scale"):
                    scale = float(spec["floor_scale"])
                    return lambda v: v * scale
        return lambda v: v
    stage = run / "stage.json"
    return _cached("searched_scale", stage, build) if stage.exists() else (lambda v: v)


def trace_payload(run, steps, policy):
    starts, c, meta = trace(run, steps, policy)
    to_cells = _searched_to_cells(run)
    eps = {(e["seed"], e["agent"]): e for e in episodes(run, steps, policy)}
    blocks = []
    get = lambda k, a, b, nd: [round(float(v), nd) for v in c[k][a:b]] if k in c else None
    for i in range(len(starts) - 1):
        a, b = int(starts[i]), int(starts[i + 1])
        if b - a < 1:
            continue
        seed = int(c["seed"][a]) if "seed" in c else i
        agent = int(c["agent"][a]) if "agent" in c else 0
        blocks.append({
            "seed": seed, "agent": agent, "episode": eps.get((seed, agent)),
            "x": get("x", a, b, 1), "y": get("y", a, b, 1), "z": get("z", a, b, 1),
            "yaw": get("yaw", a, b, 2), "cam": get("cam_yaw", a, b, 2),
            "moving": c["moving"][a:b].astype(int).tolist() if "moving" in c else None,
            "wall": c["against_wall"][a:b].astype(int).tolist() if "against_wall" in c else None,
            "fwd": c["held_forward"][a:b].astype(int).tolist() if "held_forward" in c else None,
            "strafe": c["held_strafe"][a:b].astype(int).tolist() if "held_strafe" in c else None,
            "reward": get("reward", a, b, 3),
            "flag": c["flag_pixels"][a:b].astype(int).tolist() if "flag_pixels" in c else None,
            # The map's searched cells over the episode (the coverage curve; traces from decision 0030 on).
            # ... as 1-yd cells looked at so far this episode (the scalar's scale undone, _searched_to_cells).
            "searched": ([int(round(to_cells(float(v)))) if np.isfinite(v) else None for v in c["searched"][a:b]]
                         if "searched" in c else None),
        })
    return {"step_seconds": meta.get("step_seconds", 0.25), "blocks": blocks}


# ---- backgrounds from the baked scene -------------------------------------------------------------------------------

def scene_triangles(scenes_dir, map_id):
    path = scenes_dir / f"{map_id:03d}.scene"

    def build():
        b = path.read_bytes()
        if b[:4] != b"ABSC":
            raise ValueError(f"{path} is not a baked scene")
        count = struct.unpack_from("<I", b, 72)[0]
        offset = struct.unpack_from("<Q", b, 256)[0]
        t = np.frombuffer(b, dtype="<f4", count=count * 9, offset=offset).reshape(count, 3, 3)
        v0, e1, e2 = t[:, 0], t[:, 1], t[:, 2]
        n = np.cross(e1, e2)
        n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
        return np.stack([v0, v0 + e1, v0 + e2], axis=1), n
    return _cached("scene", path, build)


GRID_SIZE, GRID_CELLS = 533.3333, 128    # the terrain's grids (Camera.h GridU): u = 128 * (32 - x / 533.3333)
WATER, LAVA = 1, 2                       # liquid kinds as the viewer draws them
CEILING_NZ = -0.3                        # a face whose normal points this much down is a ceiling or overhang


def scene_liquids(scenes_dir, map_id):
    """The scene's liquid surfaces (BakedScene.h): the models' liquid triangles with their kind byte (bits 0-1:
    0 water, 1 ocean, 2 magma, 3 slime), and the terrain's liquid tiles (a level and a kind per cell: 1 water,
    3 magma or slime)."""
    path = scenes_dir / f"{map_id:03d}.scene"

    def build():
        b = path.read_bytes()
        section = lambda slot: struct.unpack_from("<QQ", b, 256 + 16 * slot)
        count = struct.unpack_from("<I", b, 80)[0]
        tris, kinds = np.zeros((0, 3, 3), np.float32), np.zeros(0, np.uint8)
        if count:
            t = np.frombuffer(b, "<f4", count=count * 9, offset=section(4)[0]).reshape(count, 3, 3)
            tris = np.stack([t[:, 0], t[:, 0] + t[:, 1], t[:, 0] + t[:, 2]], axis=1)
            raw = np.frombuffer(b, np.uint8, count=count, offset=section(5)[0])
            kinds = np.where((raw & 3) >= 2, LAVA, WATER).astype(np.uint8)
        tiles = {}
        index_off, index_bytes = section(9)
        liquid_off = section(11)[0]
        cells = GRID_CELLS * GRID_CELLS
        for k in range(index_bytes // 36):
            tx, ty, _flags, _mx, _fl, _hi, _ho, li = struct.unpack_from("<iiIffIII", b, index_off + 36 * k)
            if li == 0xFFFFFFFF:
                continue
            base = liquid_off + li * cells * 5
            level = np.frombuffer(b, "<f4", count=cells, offset=base).reshape(GRID_CELLS, GRID_CELLS)
            kind = np.frombuffer(b, np.uint8, count=cells, offset=base + cells * 4).reshape(GRID_CELLS, GRID_CELLS)
            tiles[(tx, ty)] = (level, np.where(kind == 0, 0, np.where(kind == 3, LAVA, WATER)).astype(np.uint8))
        return tris, kinds, tiles
    return _cached("liquids", path, build)


def _barycentric(p, x0, y1, w, h):
    """The pixels a triangle covers: (r0, c0, inside mask, a, b, c) or None."""
    cx, cy = (p[:, 0] - x0) / PX, (y1 - p[:, 1]) / PX        # north up
    c0, c1 = int(max(0, math.floor(cx.min()))), int(min(w - 1, math.ceil(cx.max())))
    r0, r1 = int(max(0, math.floor(cy.min()))), int(min(h - 1, math.ceil(cy.max())))
    if c1 < c0 or r1 < r0:
        return None
    gc, gr = np.meshgrid(np.arange(c0, c1 + 1) + 0.5, np.arange(r0, r1 + 1) + 0.5)
    ax, ay, bx, by, qx, qy = cx[0], cy[0], cx[1], cy[1], cx[2], cy[2]
    d = (by - qy) * (ax - qx) + (qx - bx) * (ay - qy)
    if abs(d) < 1e-9:
        return None
    a = ((by - qy) * (gc - qx) + (qx - bx) * (gr - qy)) / d
    bb = ((qy - ay) * (gc - qx) + (ax - qx) * (gr - qy)) / d
    cc = 1 - a - bb
    inside = (a >= -0.02) & (bb >= -0.02) & (cc >= -0.02)
    return (r0, c0, inside, a, bb, cc) if inside.any() else None


def liquid_layer(scenes_dir, map_id, x0, x1, y0, y1, zlo, zhi, w, h):
    """The top liquid surface per pixel inside the height band: its level (-inf for none) and kind."""
    tris, kinds, tiles = scene_liquids(scenes_dir, map_id)
    level = np.full((h, w), -np.inf, dtype=np.float32)
    kind = np.zeros((h, w), dtype=np.uint8)
    terrain = np.zeros((h, w), dtype=bool)      # the top surface is terrain liquid (its ground is not drawn)
    if len(tris):
        tz, txy = tris[:, :, 2], tris[:, :, :2]
        keep = (tz.max(1) >= zlo) & (tz.min(1) <= zhi) & \
               (txy[:, :, 0].max(1) >= x0) & (txy[:, :, 0].min(1) <= x1) & \
               (txy[:, :, 1].max(1) >= y0) & (txy[:, :, 1].min(1) <= y1)
        for i in np.nonzero(keep)[0]:
            hit = _barycentric(tris[i], x0, y1, w, h)
            if hit is None:
                continue
            r0, c0, inside, a, bb, cc = hit
            p = tris[i]
            zz = a * p[0, 2] + bb * p[1, 2] + cc * p[2, 2]
            ok = inside & (zz >= zlo) & (zz <= zhi)
            sub_l = level[r0:r0 + ok.shape[0], c0:c0 + ok.shape[1]]
            sub_k = kind[r0:r0 + ok.shape[0], c0:c0 + ok.shape[1]]
            higher = ok & (zz > sub_l)
            sub_l[higher] = zz[higher]
            sub_k[higher] = kinds[i]
            terrain[r0:r0 + ok.shape[0], c0:c0 + ok.shape[1]][higher] = False
    if tiles:
        # Each pixel's terrain cell: u, v from the pixel centre's world x, y.
        wx = x0 + (np.arange(w) + 0.5) * PX
        wy = y1 - (np.arange(h) + 0.5) * PX
        u = np.floor(GRID_CELLS * (32 - wx / GRID_SIZE)).astype(int)
        v = np.floor(GRID_CELLS * (32 - wy / GRID_SIZE)).astype(int)
        for (tx, ty), (lv, kd) in tiles.items():
            cols = np.nonzero(u // GRID_CELLS == tx)[0]
            rows = np.nonzero(v // GRID_CELLS == ty)[0]
            if not len(cols) or not len(rows):
                continue
            cu, cv = u[cols] - tx * GRID_CELLS, v[rows] - ty * GRID_CELLS
            z = lv[cu[None, :], cv[:, None]]                  # rows (y) x cols (x); index cellX * 128 + cellY
            k = kd[cu[None, :], cv[:, None]]
            ok = (k > 0) & (z >= zlo) & (z <= zhi)
            sub_l, sub_k = level[np.ix_(rows, cols)], kind[np.ix_(rows, cols)]
            higher = ok & (z > sub_l)
            sub_l[higher], sub_k[higher] = z[higher], k[higher]
            sub_t = terrain[np.ix_(rows, cols)]
            sub_t[higher] = True
            level[np.ix_(rows, cols)], kind[np.ix_(rows, cols)], terrain[np.ix_(rows, cols)] = sub_l, sub_k, sub_t
    return level, kind, terrain


def ceiling_between(tris, n, x0, y1, w, h, floor, level, want):
    """Per pixel of `want`: a downward-facing surface lies between the floor and the liquid's level there."""
    covered = np.zeros((h, w), dtype=bool)
    rows, cols = np.nonzero(want)
    if not len(rows):
        return covered
    lo, hi = float(floor[want].min()), float(level[want].max())
    wx0, wx1 = x0 + cols.min() * PX, x0 + (cols.max() + 1) * PX
    wy1, wy0 = y1 - rows.min() * PX, y1 - (rows.max() + 1) * PX
    tz, txy = tris[:, :, 2], tris[:, :, :2]
    pick = (n[:, 2] <= CEILING_NZ) & (tz.max(1) >= lo) & (tz.min(1) <= hi) & \
           (txy[:, :, 0].max(1) >= wx0) & (txy[:, :, 0].min(1) <= wx1) & \
           (txy[:, :, 1].max(1) >= wy0) & (txy[:, :, 1].min(1) <= wy1)
    for i in np.nonzero(pick)[0]:
        hit = _barycentric(tris[i], x0, y1, w, h)
        if hit is None:
            continue
        r0, c0, inside, a, bb, cc = hit
        p = tris[i]
        zz = a * p[0, 2] + bb * p[1, 2] + cc * p[2, 2]
        sl = np.s_[r0:r0 + inside.shape[0], c0:c0 + inside.shape[1]]
        covered[sl] |= inside & want[sl] & (zz > floor[sl] + 0.5) & (zz < level[sl] - 0.1)
    return covered


def render_background(scenes_dir, map_id, x0, x1, y0, y1, zlo, zhi):
    tris, n = scene_triangles(scenes_dir, map_id)
    w, h = int((x1 - x0) / PX) + 1, int((y1 - y0) / PX) + 1
    floor = np.full((h, w), -np.inf, dtype=np.float32)
    wall = np.zeros((h, w), dtype=bool)
    tz, txy = tris[:, :, 2], tris[:, :, :2]
    keep = (tz.max(1) >= zlo) & (tz.min(1) <= zhi) & \
           (txy[:, :, 0].max(1) >= x0) & (txy[:, :, 0].min(1) <= x1) & \
           (txy[:, :, 1].max(1) >= y0) & (txy[:, :, 1].min(1) <= y1)
    for i in np.nonzero(keep)[0]:
        p = tris[i]
        cx, cy = (p[:, 0] - x0) / PX, (y1 - p[:, 1]) / PX        # north up
        c0, c1 = int(max(0, math.floor(cx.min()))), int(min(w - 1, math.ceil(cx.max())))
        r0, r1 = int(max(0, math.floor(cy.min()))), int(min(h - 1, math.ceil(cy.max())))
        if c1 < c0 or r1 < r0:
            continue
        gc, gr = np.meshgrid(np.arange(c0, c1 + 1) + 0.5, np.arange(r0, r1 + 1) + 0.5)
        ax, ay, bx, by, qx, qy = cx[0], cy[0], cx[1], cy[1], cx[2], cy[2]
        d = (by - qy) * (ax - qx) + (qx - bx) * (ay - qy)
        if abs(d) < 1e-9:
            if n[i, 2] < FLOOR_NZ:
                wall[np.clip(np.round(cy).astype(int), 0, h - 1), np.clip(np.round(cx).astype(int), 0, w - 1)] = True
            continue
        a = ((by - qy) * (gc - qx) + (qx - bx) * (gr - qy)) / d
        bb = ((qy - ay) * (gc - qx) + (ax - qx) * (gr - qy)) / d
        cc = 1 - a - bb
        inside = (a >= -0.02) & (bb >= -0.02) & (cc >= -0.02)
        if not inside.any():
            continue
        if n[i, 2] >= FLOOR_NZ:
            zz = a * p[0, 2] + bb * p[1, 2] + cc * p[2, 2]
            ok = inside & (zz >= zlo) & (zz <= zhi)
            sub = floor[r0:r1 + 1, c0:c1 + 1]
            np.maximum(sub, np.where(ok, zz, -np.inf), out=sub)
        else:
            wall[r0:r1 + 1, c0:c1 + 1] |= inside
    img = np.zeros((h, w, 4), dtype=np.uint8)
    has = np.isfinite(floor)
    if has.any():
        lo, hi = np.percentile(floor[has], [2, 98])
        t = np.clip((floor - lo) / max(hi - lo, 1e-3), 0, 1)
        base, top = np.array([86, 96, 112], np.float32), np.array([206, 212, 222], np.float32)
        rgb = base + (top - base) * t[..., None]
        img[has, :3] = rgb[has].astype(np.uint8)
        img[has, 3] = 255
    shade = wall & has
    img[shade, :3] = (img[shade, :3] * 0.45).astype(np.uint8)
    edge = wall & ~has
    img[edge] = (52, 56, 64, 255)
    # Water and lava, where the liquid's surface is at or above the floor there (a bridge over lava stays a bridge).
    # A model's liquid volume runs on under the rock around a pool, so it is drawn only over its own basin: a floor
    # below it with no ceiling between (not over empty space, nor over a tunnel that runs beneath it). Terrain
    # liquid's ground is terrain, which this drawing leaves out, so it is drawn wherever no floor stands above it.
    level, kind, terrain = liquid_layer(scenes_dir, map_id, x0, x1, y0, y1, zlo, zhi, w, h)
    with np.errstate(invalid="ignore"):
        above = ~has | (level >= floor - 0.25)
    covered = ceiling_between(tris, n, x0, y1, w, h, floor, level, (kind > 0) & ~terrain & has & above)
    show = (kind > 0) & above & (terrain | (has & ~covered))
    for k, colour, alpha in ((WATER, (52, 128, 214), 0.6), (LAVA, (255, 96, 24), 0.88)):
        m = show & (kind == k)
        if not m.any():
            continue
        under = np.where(img[m, 3:4] > 0, img[m, :3].astype(np.float32), np.float32(30))
        img[m, :3] = (under * (1 - alpha) + np.array(colour, np.float32) * alpha).astype(np.uint8)
        img[m, 3] = 255
    buf = io.BytesIO()
    Image.fromarray(img, "RGBA").save(buf, "PNG", optimize=True)
    return buf.getvalue()


def background(run, scenes_dir, steps, policy, map_id, zlo=None, zhi=None):
    """The map's background for this evaluation's routes: bounds and height band from the routes (or the band asked
    for), rendered once and kept in runs/<run>/viz/backgrounds/."""
    starts, c, _ = trace(run, steps, policy)
    eps = {(e["seed"], e["agent"]): e for e in episodes(run, steps, policy)}
    xs, ys, zs = [], [], []
    for i in range(len(starts) - 1):
        a, b = int(starts[i]), int(starts[i + 1])
        if b <= a:
            continue
        e = eps.get((int(c["seed"][a]), int(c["agent"][a]) if "agent" in c else 0))
        if not e or e["map"] != map_id:
            continue
        xs.append(c["x"][a:b]); ys.append(c["y"][a:b]); zs.append(c["z"][a:b])
    if not xs:
        raise LookupError(f"no routes on map {map_id} in this evaluation")
    xs, ys, zs = np.concatenate(xs), np.concatenate(ys), np.concatenate(zs)
    # The whole searched map, not only where this evaluation's bots went (a bot that barely moved would leave most of
    # it undrawn): every episode of this evaluation on this map adds its object and end positions (its objects span the
    # rung's rooms).
    px_, py_, pz_ = map_points(run, steps, policy, map_id)
    ax, ay = np.concatenate([xs, px_]), np.concatenate([ys, py_])
    x0, x1 = math.floor(ax.min() - MARGIN), math.ceil(ax.max() + MARGIN)
    y0, y1 = math.floor(ay.min() - MARGIN), math.ceil(ay.max() + MARGIN)
    route_lo, route_hi = float(np.percentile(zs, 1)), float(np.percentile(zs, 99))
    if len(pz_):
        route_lo, route_hi = min(route_lo, float(pz_.min())), max(route_hi, float(pz_.max()))
    band_lo = route_lo - 4 if zlo is None else zlo
    band_hi = route_hi + 3 if zhi is None else zhi
    band_lo, band_hi = math.floor(band_lo), math.ceil(band_hi)
    # The trailing tag names the drawing: a background drawn before liquids were (no tag) is drawn again.
    name = f"{map_id:03d}_{x0}_{x1}_{y0}_{y1}_{band_lo}_{band_hi}_liq.png".replace("-", "m")
    out_dir = run / "viz" / "backgrounds"
    path = out_dir / name
    if not path.exists():
        png = render_background(scenes_dir, map_id, x0, x1, y0, y1, band_lo, band_hi)
        out_dir.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        tmp.write_bytes(png)
        tmp.replace(path)
    tris, kinds, tiles = scene_liquids(scenes_dir, map_id)
    tile_kinds = {int(k) for _, kd in tiles.values() for k in np.unique(kd) if k}
    return {"map": map_id, "name": MAP_NAMES.get(map_id, f"map {map_id}"), "x0": x0, "x1": x1, "y0": y0, "y1": y1,
            "px": PX, "zlo": band_lo, "zhi": band_hi, "zmin": math.floor(float(zs.min())),
            "zmax": math.ceil(float(zs.max())), "image": f"/runs/{run.name}/viz/backgrounds/{name}",
            "water": bool((kinds == WATER).any()) or WATER in tile_kinds,
            "lava": bool((kinds == LAVA).any()) or LAVA in tile_kinds}


# ---- evaluation videos ----------------------------------------------------------------------------------------------
#
# The sim films the learner's own evaluation (AnimusForge.cpp: mode 1, no baseline) into runs/<run>/videos/<env steps>/:
# the same episodes as eval_trace_<env steps>_learner.npz, one animated PNG per sampled seed (one frame per decision,
# the camera image with the map inset) and a JSON sidecar naming its seed and agent.

VIDEO_POLICY = "learner"
_frames_lock = threading.Lock()
_frames = {}                 # (path, mtime) -> list of JPEG frames; the few most recent videos only
_decoding = {}               # (path, mtime) -> the lock its one decode holds
FRAME_CACHE = 4


FOLDER_RE = re.compile(r"\d+(-heldout\d+)?")


def _arena_names(run):
    """stage.json's arenas, by index (a held-out video's sidecar names its arena's index)."""
    def build():
        try:
            return [a.get("name") for a in json.loads((run / "stage.json").read_text()).get("arenas") or ()]
        except (OSError, ValueError):
            return []
    stage = run / "stage.json"
    return _cached("arenas", stage, build) if stage.exists() else []


def media(run, steps, policy):
    """The videos of this evaluation, by sidecar: [{seed, agent, outcome, frames, fps, folder, file, url}]. The learner's
    are in videos/<steps>/; a held-out arena's (policy heldout_<arena>) in videos/<steps>-heldout<N>/, told apart by
    the sidecar's arena index (every route is filmed since 2026-10-11)."""
    if policy == VIDEO_POLICY:
        folders = [run / "videos" / str(steps)]
        arena = None
    elif policy.startswith("heldout_"):
        folders = sorted((run / "videos").glob(f"{steps}-heldout*"))
        arena = policy[len("heldout_"):]
    else:
        return []
    names = _arena_names(run) if arena else []
    out = []
    for folder in folders:
        for side in sorted(folder.glob("*.json")) if folder.is_dir() else []:
            if side.name == "index.json":
                continue
            try:
                s = json.loads(side.read_text())
            except (OSError, ValueError):
                continue
            if not (folder / s.get("file", "")).is_file():
                continue
            if arena is not None:
                index = int(_num((s.get("measures") or {}).get("arena"), -1))
                if not (0 <= index < len(names)) or names[index] != arena:
                    continue
            out.append({"seed": int(s.get("seed", -1)), "agent": int(s.get("agent", 0)), "outcome": s.get("outcome"),
                        "frames": int(s.get("frames", 0)), "fps": s.get("fps", 4), "folder": folder.name,
                        "file": s["file"], "dropped": int(s.get("dropped_frames", 0)),
                        "truncated": bool(s.get("truncated")),
                        "url": f"/runs/{run.name}/videos/{folder.name}/{s['file']}"})
    return out


def video_frame(run, folder, file, index):
    """Frame `index` of a video as JPEG. The whole video is decoded once (an animated PNG composes each frame from
    the ones before it) and kept for the next requests."""
    if not re.fullmatch(r"[A-Za-z0-9_.-]+\.png", file) or not FOLDER_RE.fullmatch(str(folder)):
        raise LookupError("bad video name")
    path = run / "videos" / str(folder) / file
    key = (str(path), path.stat().st_mtime)
    with _frames_lock:
        frames = _frames.get(key)
        decoding = _decoding.setdefault(key, threading.Lock())
    if frames is None:
        with decoding:                       # the player's prefetch asks for many frames at once: decode once
            with _frames_lock:
                frames = _frames.get(key)
            if frames is None:
                frames = []
                with Image.open(path) as im:
                    for k in range(getattr(im, "n_frames", 1)):
                        im.seek(k)
                        buf = io.BytesIO()
                        im.convert("RGB").save(buf, "JPEG", quality=85)
                        frames.append(buf.getvalue())
                with _frames_lock:
                    _frames[key] = frames
                    while len(_frames) > FRAME_CACHE:
                        _decoding.pop(old := next(iter(_frames)), None)
                        _frames.pop(old)
    return frames[max(0, min(len(frames) - 1, index))]


# ---- HTTP -----------------------------------------------------------------------------------------------------------

class Handler(SimpleHTTPRequestHandler):
    runs_dir: Path = None
    scenes_dir: Path = None

    def __init__(self, *a, **k):
        super().__init__(*a, directory=str(STATIC), **k)

    def log_message(self, fmt, *args):
        pass

    def end_headers(self):
        if getattr(self, "_static", False):
            self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def _json(self, value, status=HTTPStatus.OK):
        body = json.dumps(value, separators=(",", ":")).encode()
        gz = "gzip" in (self.headers.get("Accept-Encoding") or "") and len(body) > 2048
        if gz:
            body = gzip.compress(body, 5)
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        if gz:
            self.send_header("Content-Encoding", "gzip")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _run(self, q):
        name = (q.get("run") or [""])[0]
        run = (self.runs_dir / name).resolve()
        if not name or run.parent != self.runs_dir.resolve() or not run.is_dir():
            raise LookupError("no such run")
        return run

    def do_GET(self):
        url = urlparse(self.path)
        q = parse_qs(url.query)
        try:
            if url.path == "/api/runs":
                return self._json(runs_index(self.runs_dir))
            if url.path == "/api/frame":
                run = self._run(q)
                # The video's folder: videos/<steps>/ (the learner's), or a held-out arena's <steps>-heldout<N>/.
                folder = q["folder"][0] if "folder" in q else str(int(q["steps"][0]))
                body = video_frame(run, folder, q["file"][0], int(q["i"][0]))
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "image/jpeg")
                self.send_header("Cache-Control", "max-age=86400")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if url.path in ("/api/episodes", "/api/trace", "/api/background", "/api/media"):
                run = self._run(q)
                steps = int(q["steps"][0])
                policy = q["policy"][0]
                if not re.fullmatch(r"[A-Za-z0-9_]+", policy):
                    raise LookupError("bad policy")
                if url.path == "/api/episodes":
                    return self._json(episodes(run, steps, policy))
                if url.path == "/api/trace":
                    return self._json(trace_payload(run, steps, policy))
                if url.path == "/api/media":
                    return self._json(media(run, steps, policy))
                zlo = float(q["zlo"][0]) if "zlo" in q else None
                zhi = float(q["zhi"][0]) if "zhi" in q else None
                return self._json(background(run, self.scenes_dir, steps, policy, int(q["map"][0]), zlo, zhi))
            if url.path.startswith("/runs/"):
                return self._run_file(url.path[len("/runs/"):])
            self._static = True              # the page's own files: the browser asks again after an update
            return super().do_GET()
        except (LookupError, KeyError, ValueError, OSError) as e:
            return self._json({"error": str(e)}, HTTPStatus.BAD_REQUEST)

    def _run_file(self, rel):
        """Only a run's viz/ files (the rendered backgrounds) and its videos/ are served."""
        parts = rel.split("/")
        if len(parts) < 3 or parts[1] not in ("viz", "videos") or ".." in parts:
            raise LookupError("not served")
        path = (self.runs_dir / rel).resolve()
        if not str(path).startswith(str(self.runs_dir.resolve())) or not path.is_file():
            raise LookupError("not found")
        body = path.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "image/png" if path.suffix == ".png" else "application/octet-stream")
        self.send_header("Cache-Control", "max-age=86400")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--runs", type=Path, default=REPO / "var/animus-forge/shared/runs",
                    help="the runs folder (default: this checkout's var/animus-forge/shared/runs)")
    ap.add_argument("--scenes", type=Path, default=REPO / "env/dist/etc/modules/animus/scenes",
                    help="the baked scenes (AnimusForge.DataDir/scenes; default: this checkout's)")
    ap.add_argument("--host", default="0.0.0.0", help="address to listen on (default all: the LAN can open it)")
    ap.add_argument("--port", type=int, default=8770)
    args = ap.parse_args()
    Handler.runs_dir, Handler.scenes_dir = args.runs, args.scenes
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Forge route viewer on http://{args.host}:{args.port}/  (runs {args.runs}, scenes {args.scenes})")
    server.serve_forever()


if __name__ == "__main__":
    main()
