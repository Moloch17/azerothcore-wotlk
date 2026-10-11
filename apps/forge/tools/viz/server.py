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


def episodes(run, steps, policy):
    """The evaluation's episodes, keyed by (seed, agent), with the fields the viewer shows."""
    path = run / "eval_episodes.jsonl"

    def build():
        rows = {}
        with open(path) as f:
            for line in f:
                try:
                    r = json.loads(line)
                except ValueError:
                    continue
                rows.setdefault((int(r.get("env_steps", -1)), r.get("policy")), []).append(r)
        return rows
    rows = _cached("episodes", path, build).get((steps, policy), [])
    cats = _categories(run)
    rooms, objects = cats.get("seek_room", []), cats.get("seek_object", [])
    out = []
    for r in rows:
        room_i, obj_i = _num(r.get("seek_room")), _num(r.get("seek_object"))
        out.append({
            "seed": int(r.get("seed", -1)), "agent": int(r.get("agent", 0) or 0),
            "map": int(_num(r.get("seek_map"), 0)),
            "found": bool(_num(r.get("found"), 0)), "timed_out": bool(_num(r.get("timed_out"), 0)),
            "find_seconds": _num(r.get("find_seconds")),
            "class": r.get("layout") or r.get("class") or "?", "race": RACES.get(int(_num(r.get("race"), 0)), r.get("race")), "level": _num(r.get("level")),
            "room": rooms[int(room_i)] if room_i is not None and 0 <= int(room_i) < len(rooms) else None,
            "object": objects[int(obj_i)] if obj_i is not None and 0 <= int(obj_i) < len(objects) else None,
            "object_xyz": [_num(r.get("object_x")), _num(r.get("object_y")), _num(r.get("object_z"))],
            "end_xyz": [_num(r.get("end_x")), _num(r.get("end_y")), _num(r.get("end_z"))],
            "pin_events": _num(r.get("pin_events")), "wall_seconds": _num(r.get("wall_seconds")),
            "return": _num(r.get("return")), "rung": _num(r.get("seek_rung")),
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


def trace_payload(run, steps, policy):
    starts, c, meta = trace(run, steps, policy)
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
    x0, x1 = math.floor(xs.min() - MARGIN), math.ceil(xs.max() + MARGIN)
    y0, y1 = math.floor(ys.min() - MARGIN), math.ceil(ys.max() + MARGIN)
    band_lo = float(np.percentile(zs, 1)) - 4 if zlo is None else zlo
    band_hi = float(np.percentile(zs, 99)) + 3 if zhi is None else zhi
    band_lo, band_hi = math.floor(band_lo), math.ceil(band_hi)
    name = f"{map_id:03d}_{x0}_{x1}_{y0}_{y1}_{band_lo}_{band_hi}.png".replace("-", "m")
    out_dir = run / "viz" / "backgrounds"
    path = out_dir / name
    if not path.exists():
        png = render_background(scenes_dir, map_id, x0, x1, y0, y1, band_lo, band_hi)
        out_dir.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        tmp.write_bytes(png)
        tmp.replace(path)
    return {"map": map_id, "name": MAP_NAMES.get(map_id, f"map {map_id}"), "x0": x0, "x1": x1, "y0": y0, "y1": y1,
            "px": PX, "zlo": band_lo, "zhi": band_hi, "zmin": math.floor(float(zs.min())),
            "zmax": math.ceil(float(zs.max())), "image": f"/runs/{run.name}/viz/backgrounds/{name}"}


# ---- HTTP -----------------------------------------------------------------------------------------------------------

class Handler(SimpleHTTPRequestHandler):
    runs_dir: Path = None
    scenes_dir: Path = None

    def __init__(self, *a, **k):
        super().__init__(*a, directory=str(STATIC), **k)

    def log_message(self, fmt, *args):
        pass

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
            if url.path in ("/api/episodes", "/api/trace", "/api/background"):
                run = self._run(q)
                steps = int(q["steps"][0])
                policy = q["policy"][0]
                if not re.fullmatch(r"[A-Za-z0-9_]+", policy):
                    raise LookupError("bad policy")
                if url.path == "/api/episodes":
                    return self._json(episodes(run, steps, policy))
                if url.path == "/api/trace":
                    return self._json(trace_payload(run, steps, policy))
                zlo = float(q["zlo"][0]) if "zlo" in q else None
                zhi = float(q["zhi"][0]) if "zhi" in q else None
                return self._json(background(run, self.scenes_dir, steps, policy, int(q["map"][0]), zlo, zhi))
            if url.path.startswith("/runs/"):
                return self._run_file(url.path[len("/runs/"):])
            return super().do_GET()
        except (LookupError, KeyError, ValueError, OSError) as e:
            return self._json({"error": str(e)}, HTTPStatus.BAD_REQUEST)

    def _run_file(self, rel):
        """Only a run's viz/ files are served (the rendered backgrounds)."""
        parts = rel.split("/")
        if len(parts) < 3 or parts[1] != "viz" or ".." in parts:
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
