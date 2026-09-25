"""A local dashboard for a forge run: the config it is running under, and what it is doing right now.

The worldserver container starts it (apps/docker/forge-worldserver.sh) and publishes it on
http://localhost:18800, next to TensorBoard. To run one by hand, in the container or on the host:

    python3 apps/forge/python/animus/dashboard.py [--port 8800] [--host 0.0.0.0] [--runs DIR]

Reads the files the sim and the learner already write (runs/<stage>/progress.json, eval.csv, metrics.csv,
eval_episodes.jsonl, finished.json) and the worldserver's own conf, for both the real runs and a `forge fast`
sweep's own tree. Reading never touches the sim, so the page is safe to start, stop and restart under a live run.
Standard library only -- it runs on the host's python without the learner's venv, and binds to the loopback address
only.

The stage controls (pause, resume, skip, cancel) are the one thing that does talk to the sim, over SOAP, and they
exist only when `--soap-auth` names a readable `user:password` file. Without it the page is exactly as read-only as
it always was. The commands are a fixed list mapped onto `forge <name>`: the request carries a name, never a
command string, so nothing the page is tricked into sending can become a different console command.

TensorBoard (http://localhost:16006) plots the same scalars in more depth; this page answers the questions it cannot:
what config is this run using, which stage of the plan is live, and how is each class and build doing in the last
evaluation.
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import os
import re
import time
import base64
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from xml.etree import ElementTree

ROOT = Path(__file__).resolve().parents[4]          # <repo>/apps/forge/python/animus/dashboard.py
DEFAULT_RUNS = ROOT / "var" / "animus-forge" / "runs"
DEFAULT_CONF = ROOT / "env" / "dist" / "etc" / "worldserver.conf"
# A tuned installation from the module days keeps its keys here; the server reads it after worldserver.conf.
LEGACY_CONF = ROOT / "env" / "dist" / "etc" / "modules" / "mod_animus_forge.conf"
DIST_CONF = ROOT / "src" / "server" / "apps" / "worldserver" / "worldserver.conf.dist"
# The sim's SOAP endpoint (worldserver.conf SOAP.IP/SOAP.Port). Loopback: the dashboard runs in the
# same container as the worldserver.
DEFAULT_SOAP_URL = "http://127.0.0.1:7878/"

# What to plot. A fixed list cannot serve 24 stages: "killed" is the whole point of a duel and says nothing in the
# healing drill, the flag stages live and die by captures, and a travel stage is about arriving. So each run's
# series are picked out of its own metrics.csv -- a column that never moves in this run is a flat line taking the
# place of one the stage actually teaches.
#
# PLOT_ALWAYS is the run's health rather than its subject, and comes first whenever it varies.
PLOT_ALWAYS = ["reward_per_decision", "entropy", "episode_difficulty"]

# Preferred order for the rest. A column not named here can still be plotted -- a new episode_ column needs no
# change here -- it just sorts after these, by how much it moves.
PLOT_PREFERRED = [
    "episode_killed", "episode_died", "episode_timed_out", "episode_health_left",
    "episode_flag_captures", "episode_flag_pickups", "episode_flag_returns", "episode_match_won",
    "episode_owner_deaths", "episode_teammates_died", "episode_wipes", "episode_pulls_cleared",
    "episode_healing_done", "episode_overheal_share", "episode_healing_per_mana",
    "episode_damage_taken", "episode_dps", "episode_arrived", "episode_hazard_seconds",
    "episode_casts_completed", "episode_casts_cancelled", "episode_interrupts", "episode_control_seconds",
    "episode_stealth_openers", "episode_detected", "episode_escaped",
]

# Bookkeeping rather than behaviour: what class the seat rolled says nothing about what the stage taught.
PLOT_NEVER = {
    "update", "env_steps", "env_steps_per_sec", "update_seconds", "episodes",
    "episode_level", "episode_race", "episode_class", "episode_spec",
    "episode_present", "episode_talent_plan", "episode_opponent", "episode_opponent_seat",
    "episode_unspent_talent_points", "episode_equipped_items", "episode_form_at_end",
}

MAX_SERIES = 9          # what the charts grid holds without the panel becoming a wall of flat lines


def series_label(column: str) -> str:
    """`episode_flag_captures` -> `flag captures`, which is what a chart has room for."""
    return column.replace("episode_", "").replace("_", " ")

# Per (class, build), from the last evaluation's episodes.
LAYOUT_FIELDS = ["killed", "died", "timed_out", "options_started", "option_seconds", "preparation_seconds",
                 "in_melee_share", "repeated_presses", "health_left", "interrupts", "control_seconds"]


# What a button may do, and the console command each one becomes. The page sends the key; nothing else reaches
# the sim, so the control surface is this dict and not "whatever string arrived".
COMMANDS = {
    "pause": "forge pause",
    "resume": "forge resume",
    "skip": "forge skip",
    "cancel": "forge cancel",
}

SOAP_ENVELOPE = (
    '<?xml version="1.0" encoding="utf-8"?>'
    '<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/" xmlns:ns1="urn:AC">'
    "<SOAP-ENV:Body><ns1:executeCommand><command>{command}</command></ns1:executeCommand>"
    "</SOAP-ENV:Body></SOAP-ENV:Envelope>"
)


# The one-word file beside a run's checkpoints that says which of them should seed the stage after it. The
# learner reads the same name (animus.train.seed_preference); it is duplicated rather than imported because this
# page runs on the image's python, without the learner's venv, and importing animus.train would pull in torch.
SEED_MARKER = "seed_from"
SEED_CHOICES = ("best", "latest")
SEED_DEFAULT = "latest"        # TrainConfig.seed_from


def seed_state(run_dir: Path, progress: dict) -> dict:
    """What the next stage would seed from if this run ended now, and how far behind that leaves it.

    best.pt is only rewritten by an evaluation that clears the convergence margin, so it can sit a long way behind
    latest.pt -- on a short run, where 64-episode evaluations make the margin wide, millions of steps. That gap is
    the whole reason for the choice, so it is what the page shows."""
    try:
        marked = (run_dir / SEED_MARKER).read_text().strip().lower()
    except OSError:
        marked = ""
    choice = marked if marked in SEED_CHOICES else ""

    has = {name: (run_dir / f"{name}.pt").exists() for name in SEED_CHOICES}
    # Unmarked resolves to whatever the learner's own default is (TrainConfig.seed_from, "latest"). Held here
    # rather than imported for the reason SEED_MARKER is; test_seed_from checks the two still agree, which is what
    # caught this when the default changed under the page.
    prefer = choice or SEED_DEFAULT
    other = "latest" if prefer == "best" else "best"
    resolved = prefer if has[prefer] else (other if has[other] else "")

    at = progress.get("best_env_steps") or 0
    now = progress.get("env_steps") or 0
    return {
        "choice": choice,                       # "" when the run carries no marker and the default decides
        "resolved": resolved,                   # what the next stage would actually take
        "has": has,
        "best_env_steps": at,
        "env_steps": now,
        "behind": max(0, now - at) if has["best"] else 0,
    }


def read_soap_auth(path: Path | None) -> tuple[str, str] | None:
    """`user:password` from a file, or None when there is none to read.

    A file rather than an argument or an environment variable: a password in argv is in every `ps` listing on the
    box, and the sim's own console is already reachable by anyone who can read this file."""
    if not path:
        return None
    try:
        user, _, password = path.read_text().strip().partition(":")
    except OSError:
        return None
    return (user, password) if user and password else None


def run_command(name: str, url: str, auth: tuple[str, str]) -> tuple[bool, str]:
    """Run one of COMMANDS through the sim's SOAP endpoint. Returns (ok, what the console said)."""
    command = COMMANDS.get(name)
    if not command:
        return False, f"{name} is not a command this page can run"

    token = base64.b64encode(f"{auth[0]}:{auth[1]}".encode()).decode()
    request = urllib.request.Request(
        url,
        data=SOAP_ENVELOPE.format(command=command).encode(),
        headers={"Content-Type": "application/xml", "Authorization": f"Basic {token}"},
    )
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            body = response.read().decode(errors="replace")
    except urllib.error.HTTPError as error:
        detail = error.read().decode(errors="replace")
        # A fault carries the reason the command was refused, which is more use than the status code.
        return False, _soap_text(detail) or f"the sim refused it (HTTP {error.code})"
    except OSError as error:
        return False, f"the sim did not answer ({error.__class__.__name__}); is SOAP.Enabled set?"

    return True, _soap_text(body) or "done"


def _soap_text(body: str) -> str:
    """The <result> (or fault reason) out of a SOAP body, as text."""
    try:
        root = ElementTree.fromstring(body)
    except ElementTree.ParseError:
        return ""
    for tag in ("result", "faultstring", "{http://schemas.xmlsoap.org/soap/envelope/}Text"):
        found = root.iter(tag)
        for element in found:
            if element.text:
                return element.text.strip()
    return ""


def _read_json(path: Path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return None


class Cache:
    """Parsed files, keyed by path and invalidated by (mtime, size): a poll every few seconds must not re-read a
    100 MB episode log that has not changed."""

    def __init__(self):
        self._entries: dict[Path, tuple[tuple[float, int], object]] = {}

    def get(self, path: Path, parse):
        try:
            stat = path.stat()
        except OSError:
            return None
        stamp = (stat.st_mtime, stat.st_size)
        hit = self._entries.get(path)
        if hit and hit[0] == stamp:
            return hit[1]
        try:
            value = parse(path)
        except (OSError, ValueError):
            return None
        self._entries[path] = (stamp, value)
        return value


CACHE = Cache()


def parse_conf(path: Path) -> dict[str, str]:
    """`Key = value` lines of a worldserver conf, comments and blanks dropped."""
    values: dict[str, str] = {}
    for line in path.read_text(errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip().strip('"')
    return values


def parse_eval(path: Path) -> list[dict]:
    rows = []
    for row in csv.DictReader(io.StringIO(path.read_text(errors="replace"))):
        try:
            rows.append({
                "update": int(float(row["update"])),
                "env_steps": int(float(row["env_steps"])),
                "policy": row["policy"],
                "score": float(row["score"]),
                "stderr": float(row.get("stderr") or 0.0),
                "best": float(row["best"]) if row.get("best") else None,
            })
        except (KeyError, TypeError, ValueError):
            continue
    return rows


def parse_metrics(path: Path, points: int = 240) -> dict[str, list]:
    """Every numeric column of metrics.csv, thinned to `points` samples: an hour of training is thousands of
    updates and the page has a few hundred pixels to draw them in.

    All of them, not a chosen few, because which columns matter is a property of the stage rather than of the
    dashboard -- `choose_series` picks from these once they can be seen moving. The whole parse is cached against
    the file's mtime, and only the chosen series are sent to the page."""
    rows = list(csv.DictReader(io.StringIO(path.read_text(errors="replace"))))
    if not rows:
        return {}
    step = max(1, len(rows) // points)
    kept = rows[::step] + ([rows[-1]] if len(rows) > 1 else [])
    columns = [name for name in (rows[0].keys() or ()) if name and name not in PLOT_NEVER]
    series: dict[str, list] = {"env_steps": []}
    for name in columns:
        series[name] = []
    for row in kept:
        try:
            series["env_steps"].append(float(row["env_steps"]))
        except (KeyError, TypeError, ValueError):
            continue
        for name in columns:
            try:
                series[name].append(float(row[name]))
            except (KeyError, TypeError, ValueError):
                series[name].append(None)
    return series


def choose_series(metrics: dict[str, list], limit: int = MAX_SERIES) -> list[str]:
    """Which columns of this run are worth a chart, most telling first.

    A column that holds one value for the whole run taught the stage nothing that can be watched -- every stage
    carries every column, so a duel run still has `flag_captures` sitting at zero. What is left is ordered by
    PLOT_ALWAYS, then PLOT_PREFERRED, then by how much it moves relative to its own size, so a stage nobody wrote
    a preference for still gets its liveliest columns rather than the alphabetical ones."""
    moved: dict[str, float] = {}
    for name, values in metrics.items():
        if name == "env_steps" or name in PLOT_NEVER:
            continue
        seen = [v for v in values if v is not None]
        if len(seen) < 2:
            continue
        low, high = min(seen), max(seen)
        if high == low:
            continue                                   # a flat line says only that the column exists
        scale = max(abs(high), abs(low), 1e-9)
        moved[name] = (high - low) / scale

    ordered = [name for name in PLOT_ALWAYS if name in moved]
    ordered += [name for name in PLOT_PREFERRED if name in moved and name not in ordered]
    ordered += sorted((name for name in moved if name not in ordered), key=lambda n: -moved[n])
    return ordered[:limit]


def parse_live_layouts(path: Path) -> dict:
    """The newest update's per (class, build) training rows (layouts.csv). Training episodes, not an evaluation:
    sampled actions at each class and build's own ladder difficulty, which is what makes them live -- they arrive
    every update rather than every eval.every_env_steps.

    One model is a whole class, so the rows are per class and build and are shown that way: a paladin appears as
    paladin_tank and paladin_heal, which is what the eighteen layouts used to be called and what the difficulty
    ladder and the sampling weights are still keyed on."""
    rows = list(csv.DictReader(io.StringIO(path.read_text(errors="replace"))))
    if not rows:
        return {}

    newest = rows[-1].get("update")
    latest = [row for row in rows if row.get("update") == newest]
    layouts = []
    for row in latest:
        name = row.get("layout", "?")
        spec = (row.get("spec") or "").strip()
        entry = {"layout": f"{name}_{spec}" if spec else name, "class": name, "spec": spec}
        for key, value in row.items():
            if key.startswith("episode_") or key in ("episodes", "env_steps"):
                try:
                    entry[key.replace("episode_", "")] = float(value)
                except (TypeError, ValueError):
                    continue
        layouts.append(entry)

    layouts.sort(key=lambda entry: entry.get("killed", 0.0))
    return {"update": newest, "env_steps": layouts[0].get("env_steps", 0) if layouts else 0, "layouts": layouts}


def parse_layouts(path: Path) -> dict:
    """Per (class, build) means over the last evaluation in the episode log (the learner appends every one).

    Keyed on the pair rather than the model for the same reason the training rows are: a class with a build that
    holds the line and one that heals is two things to be good at, and one average over both describes neither.
    The build's name comes from the episode log, which the learner writes with it."""
    last_steps = None
    totals: dict[str, dict[str, float]] = {}
    with path.open(errors="replace") as handle:
        for line in handle:
            try:
                row = json.loads(line)
            except ValueError:
                continue
            if row.get("policy") == "fight":
                continue
            steps = row.get("env_steps", 0)
            if last_steps is None or steps > last_steps:
                last_steps, totals = steps, {}
            if steps != last_steps:
                continue
            name = row.get("layout", "?")
            spec = row.get("spec")
            if not isinstance(spec, str):
                spec = None
            entry = totals.setdefault(f"{name}_{spec}" if spec else name, {"n": 0.0})
            entry["n"] += 1
            for field in LAYOUT_FIELDS:
                entry[field] = entry.get(field, 0.0) + (row.get(field) or 0.0)
    layouts = []
    for name in sorted(totals):
        entry = totals[name]
        count = entry["n"]
        means = {field: entry.get(field, 0.0) / count for field in LAYOUT_FIELDS}
        started = means["options_started"]
        means["seconds_per_option"] = means["option_seconds"] / started if started else 0.0
        means["episodes"] = count
        means["layout"] = name
        layouts.append(means)
    return {"env_steps": last_steps or 0, "layouts": layouts}


def run_state(run_dir: Path, fast: bool = False) -> dict | None:
    progress = CACHE.get(run_dir / "progress.json", _read_json)
    finished = CACHE.get(run_dir / "finished.json", _read_json)
    if not progress and not finished:
        return None
    progress = progress or {}
    evals = CACHE.get(run_dir / "eval.csv", parse_eval) or []
    return {
        "name": run_dir.name,
        "dir": str(run_dir),
        # A fast sweep trains every stage again under its own budget, in its own directory, so the same stage
        # name exists in both. Without this the page cannot say which one it is showing.
        "fast": fast,
        "progress": progress,
        "finished": finished,
        "evals": evals,
        "updated_at": progress.get("updated_at", 0),
        "live": bool(progress) and not finished and time.time() - progress.get("updated_at", 0) < 120,
    }


def fast_runs_dir(runs_dir: Path, conf: dict) -> Path:
    """Where `forge fast` writes, beside the real runs.

    A fast sweep never touches runs/: it trains every stage again from scratch under AnimusForge.Fast.OutputDir
    (relative to AnimusForge.OutputDir, default "fast"). The dashboard was pointed at runs/ alone, so a fast run
    -- the whole curriculum, for hours -- left the page empty. Read from the conf rather than assumed, so moving
    the directory moves the dashboard with it."""
    name = (conf.get("AnimusForge.Fast.OutputDir") or "fast").strip().strip('"')
    return runs_dir.parent / name / "runs"


def collect(runs_dir: Path, conf_path: Path) -> dict:
    live = CACHE.get(conf_path, parse_conf) or {}
    legacy = conf_path.parent / "modules" / "mod_animus_forge.conf"
    if legacy.is_file():
        live = {**live, **(CACHE.get(legacy, parse_conf) or {})}
    live = {key: value for key, value in live.items() if key.startswith("AnimusForge.")}
    fast_dir = fast_runs_dir(runs_dir, live)

    runs = []
    for base, fast in ((runs_dir, False), (fast_dir, True)):
        if not base.is_dir():
            continue
        for child in sorted(base.iterdir()):
            if not child.is_dir() or child.name.startswith("_"):
                continue
            state = run_state(child, fast)
            if state:
                runs.append(state)

    runs.sort(key=lambda r: r["updated_at"], reverse=True)
    current = next((r for r in runs if r["live"]), runs[0] if runs else None)

    metrics: dict = {}
    series: list = []
    layouts: dict = {}
    live_layouts: dict = {}
    seed: dict = {}
    if current:
        run_dir = Path(current["dir"])
        metrics = CACHE.get(run_dir / "metrics.csv", parse_metrics) or {}
        chosen = choose_series(metrics)
        # Only the chosen columns cross the wire; metrics.csv is over a hundred columns wide and the page polls
        # every few seconds.
        metrics = {name: metrics[name] for name in ["env_steps", *chosen] if name in metrics}
        series = [{"key": name, "label": series_label(name)} for name in chosen]
        layouts = CACHE.get(run_dir / "eval_episodes.jsonl", parse_layouts) or {}
        live_layouts = CACHE.get(run_dir / "layouts.csv", parse_live_layouts) or {}
        seed = seed_state(run_dir, current["progress"])

    dist = CACHE.get(DIST_CONF, parse_conf) or {}
    config = [{"key": key, "value": value, "default": dist.get(key), "changed": key in dist and dist[key] != value}
              for key, value in sorted(live.items())]
    # Keys the conf leaves out take the module's built-in default, which the dist file documents.
    config += [{"key": key, "value": value, "default": value, "changed": False, "from_dist": True}
               for key, value in sorted(dist.items()) if key not in live]

    return {
        "now": time.time(),
        "runs_dir": str(runs_dir),
        "fast_dir": str(fast_dir),
        "conf": str(conf_path),
        "runs": [{k: v for k, v in run.items() if k != "progress"} | {"progress": run["progress"]} for run in runs],
        "current": current["dir"] if current else None,
        "series": series,
        "metrics": metrics,
        "layouts": layouts,
        "live": live_layouts,
        "seed": seed,
        "config": config,
    }


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>Animus Forge</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  :root {
    --bg: #0f1115; --panel: #171a21; --line: #262b36; --text: #e6e9ef; --dim: #939cad;
    --good: #5bd6a0; --warn: #e5c07b; --bad: #e88388; --accent: #7aa2f7;
  }
  * { box-sizing: border-box; }
  body { margin: 0; background: var(--bg); color: var(--text);
         font: 14px/1.5 ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif; }
  header { display: flex; align-items: baseline; gap: 16px; flex-wrap: wrap;
           padding: 14px 20px; border-bottom: 1px solid var(--line); }
  h1 { font-size: 16px; margin: 0; font-weight: 650; letter-spacing: .01em; }
  .muted { color: var(--dim); }
  main { padding: 20px; display: grid; gap: 16px; max-width: 1400px; }
  /* A table of class and build pairs is wider than a phone: each scrolls sideways inside its panel, so the page
     itself never does. Without this the body scrolls horizontally and the cards and charts go off-screen. */
  .panel > .scroll, .panel > div[id] { overflow-x: auto; }
  .cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; }
  .card { background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 12px 14px; }
  .card .k { color: var(--dim); font-size: 11px; text-transform: uppercase; letter-spacing: .06em; }
  .card .v { font-size: 20px; font-variant-numeric: tabular-nums; margin-top: 2px; }
  .panel { background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 14px 16px; }
  .panel h2 { font-size: 13px; margin: 0 0 10px; color: var(--dim); text-transform: uppercase;
              letter-spacing: .06em; font-weight: 600; }
  table { border-collapse: collapse; width: 100%; font-variant-numeric: tabular-nums; }
  th, td { text-align: right; padding: 4px 8px; border-bottom: 1px solid var(--line); white-space: nowrap; }
  th:first-child, td:first-child { text-align: left; }
  th { color: var(--dim); font-weight: 600; font-size: 12px; position: sticky; top: 0; background: var(--panel); }
  tbody tr:hover { background: #1d222c; }
  .charts { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(320px, 100%), 1fr)); gap: 14px; }
  .chart { height: 150px; width: 100%; }
  .bar { height: 6px; background: var(--line); border-radius: 3px; overflow: hidden; margin-top: 6px; }
  .bar > div { height: 100%; background: var(--accent); }
  input[type=search] { background: #11141a; color: var(--text); border: 1px solid var(--line);
                       border-radius: 8px; padding: 6px 10px; width: 260px; max-width: 100%; }
  label { color: var(--dim); font-size: 12px; margin-left: 12px; }
  .scroll { max-height: 420px; overflow: auto; }
  .good { color: var(--good); } .warn { color: var(--warn); } .bad { color: var(--bad); }
  .dot { width: 8px; height: 8px; border-radius: 50%; display: inline-block; margin-right: 6px; }
  .changed td:first-child::after { content: " changed"; color: var(--warn); font-size: 11px; }
  .tag { font-size: 10px; text-transform: uppercase; letter-spacing: .06em; color: var(--bg);
         background: var(--accent); border-radius: 4px; padding: 1px 5px; vertical-align: 1px; }
  tr.current td { background: #1a2030; }
  .controls { display: inline-flex; gap: 6px; flex-wrap: wrap; }
  .controls button { background: #1b2030; color: var(--text); border: 1px solid var(--line);
                     border-radius: 7px; padding: 5px 11px; font: inherit; font-size: 12px; cursor: pointer; }
  .controls button:hover:not(:disabled) { background: #232a3a; border-color: var(--accent); }
  .controls button:disabled { opacity: .45; cursor: default; }
  .controls button.danger:hover:not(:disabled) { border-color: var(--bad); color: var(--bad); }
  .said { max-width: 42ch; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: 12px; }
  .said.bad { color: var(--bad); } .said.good { color: var(--good); }
  .seedrow { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
  .seedrow button { background: #1b2030; color: var(--text); border: 1px solid var(--line);
                    border-radius: 7px; padding: 5px 11px; font: inherit; font-size: 12px; cursor: pointer; }
  .seedrow button:hover { border-color: var(--accent); }
  .seedrow button.on { border-color: var(--accent); background: #22304a; }
  .seedrow .why { font-size: 12px; }
  @media (max-width: 700px) {
    main { padding: 12px; gap: 12px; }
    header { padding: 12px; gap: 8px; }
    .panel { padding: 10px 12px; }
    .cards { grid-template-columns: repeat(auto-fit, minmax(118px, 1fr)); gap: 8px; }
    .card { padding: 8px 10px; }
    .card .v { font-size: 17px; }
    th, td { padding: 3px 6px; font-size: 12px; }
    .scroll { max-height: 320px; }
    /* The filter and its checkbox wrap under the heading rather than pushing it off the edge. */
    .panel h2 { display: flex; flex-wrap: wrap; align-items: center; gap: 6px; }
    label { margin-left: 0; }
  }
</style></head>
<body>
<header>
  <h1>Animus Forge</h1>
  <span id="stage" class="muted"></span>
  <span style="flex:1"></span>
  <span id="controls" class="controls"></span>
  <span id="said" class="muted said"></span>
  <span id="clock" class="muted"></span>
</header>
<main>
  <div class="cards" id="cards"></div>
  <div class="panel"><h2>Training</h2><div class="charts" id="charts"></div></div>
  <div class="panel"><h2>Evaluations</h2><div id="evals"></div></div>
  <div class="panel"><h2>Class and build, right now <span id="livesteps" class="muted"></span></h2>
    <div class="scroll"><table id="live"></table></div></div>
  <div class="panel"><h2>Class and build, last evaluation <span id="layoutsteps" class="muted"></span></h2>
    <div class="scroll"><table id="layouts"></table></div></div>
  <div class="panel"><h2>Seeding the next stage</h2><div id="seed"></div></div>
  <div class="panel"><h2>Runs</h2><div class="scroll"><table id="runs"></table></div></div>
  <div class="panel"><h2>Config
      <input type="search" id="filter" placeholder="filter keys and values">
      <label><input type="checkbox" id="changedonly"> changed from the dist default only</label></h2>
    <div class="scroll"><table id="config"></table></div></div>
</main>
<script>
const fmt = (v, d = 2) => v === null || v === undefined || Number.isNaN(v) ? "-" : Number(v).toFixed(d);
const steps = v => v >= 1e6 ? (v / 1e6).toFixed(1) + "M" : v >= 1e3 ? (v / 1e3).toFixed(0) + "k" : String(v ?? 0);
const dur = s => { if (!s || s < 0) return "-"; const h = Math.floor(s / 3600), m = Math.round(s % 3600 / 60);
                   return h ? `${h}h ${m}m` : `${m}m`; };

function card(k, v, cls) { return `<div class="card"><div class="k">${k}</div><div class="v ${cls || ""}">${v}</div></div>`; }

function chart(series, xs, ys, label) {
  const w = 320, h = 150, pad = 26;
  const pts = xs.map((x, i) => [x, ys[i]]).filter(p => p[1] !== null && p[1] !== undefined);
  if (pts.length < 2) return `<svg class="chart" viewBox="0 0 ${w} ${h}"><text x="8" y="20" fill="#939cad">${label}</text></svg>`;
  const xmin = Math.min(...pts.map(p => p[0])), xmax = Math.max(...pts.map(p => p[0]));
  const ymin = Math.min(...pts.map(p => p[1])), ymax = Math.max(...pts.map(p => p[1]));
  const sx = x => pad + (w - pad - 6) * (xmax === xmin ? 1 : (x - xmin) / (xmax - xmin));
  const sy = y => h - 18 - (h - 34) * (ymax === ymin ? 0.5 : (y - ymin) / (ymax - ymin));
  const d = pts.map((p, i) => `${i ? "L" : "M"}${sx(p[0]).toFixed(1)},${sy(p[1]).toFixed(1)}`).join("");
  const last = pts[pts.length - 1][1];
  return `<svg class="chart" viewBox="0 0 ${w} ${h}">
    <line x1="${pad}" y1="${h - 18}" x2="${w - 6}" y2="${h - 18}" stroke="#262b36"/>
    <path d="${d}" fill="none" stroke="#7aa2f7" stroke-width="1.6"/>
    <text x="4" y="14" fill="#939cad" font-size="11">${label}</text>
    <text x="${w - 6}" y="14" fill="#e6e9ef" font-size="11" text-anchor="end">${fmt(last, 3)}</text>
    <text x="4" y="${h - 4}" fill="#939cad" font-size="10">${fmt(ymin, 2)}</text>
    <text x="${w - 6}" y="${h - 4}" fill="#939cad" font-size="10" text-anchor="end">${steps(xmax)} steps</text>
  </svg>`;
}

let state = null;

function render() {
  if (!state) return;
  const run = (state.runs || []).find(r => r.dir === state.current);
  const p = run ? run.progress || {} : {};
  const phase = run ? (run.finished ? "finished: " + (run.finished.reason || "") : p.phase || "?") : "";
  document.getElementById("stage").innerHTML = run
    ? `${run.name}${run.fast ? ` <span class="tag">fast</span>` : ""} <span class="muted">- ${phase}</span>`
    : `<span class="muted">no runs yet</span>`;
  document.getElementById("clock").textContent = new Date(state.now * 1000).toLocaleTimeString();

  const total = p.total_env_steps || 0, done = p.env_steps || 0;
  const eta = p.env_steps_per_sec && total > done ? (total - done) / p.env_steps_per_sec : null;
  const score = p.last_eval_score, base = p.baseline_score;
  const cls = score === undefined || base === undefined ? "" : score > base ? "good" : "bad";
  document.getElementById("cards").innerHTML = [
    card("steps", `${steps(done)}<div class="bar"><div style="width:${total ? Math.min(100, 100 * done / total) : 0}%"></div></div>`),
    card("steps / s", fmt(p.env_steps_per_sec, 0)),
    card("update", p.update ?? "-"),
    card("elapsed", dur(p.elapsed_seconds)),
    card("eta", dur(eta)),
    card("last eval", fmt(score, 2), cls),
    card("baseline", fmt(base, 2)),
    card("best", `${fmt(p.best_score, 2)} <span class="muted" style="font-size:12px">@${steps(p.best_env_steps || 0)}</span>`),
    card("entropy", fmt(p.entropy, 3)),
    card("approx kl", fmt(p.approx_kl, 4)),
    card("clip frac", fmt(p.clip_frac, 3)),
    card("reward / decision", fmt(p.reward_per_decision, 4)),
  ].join("");

  const m = state.metrics || {};
  document.getElementById("charts").innerHTML = (state.series || [])
    .map(s => chart(s.key, m.env_steps || [], m[s.key] || [], s.label)).join("");

  const evals = (run && run.evals || []).slice().reverse();
  document.getElementById("evals").innerHTML = evals.length ? `<table>
    <thead><tr><th>steps</th><th>policy</th><th>score</th><th>+/-</th><th>best</th></tr></thead><tbody>
    ${evals.map(e => `<tr><td>${steps(e.env_steps)}</td><td>${e.policy}</td><td>${fmt(e.score, 3)}</td>
      <td class="muted">${fmt(e.stderr, 3)}</td><td>${e.best === null ? "-" : fmt(e.best, 3)}</td></tr>`).join("")}
    </tbody></table>` : `<span class="muted">none yet</span>`;

  // What each class and build is doing in the training episodes of the newest update, not at the last evaluation.
  const LV = state.live || {};
  document.getElementById("livesteps").textContent = LV.update !== undefined
    ? `(update ${LV.update}, ${steps(LV.env_steps || 0)} steps, sampled actions)` : "(waiting for the first update)";
  const livecols = [["episodes", "eps", 0], ["killed", "kill", 2], ["died", "die", 2], ["timed_out", "t/o", 2],
                    ["difficulty", "diff", 1], ["in_melee_share", "melee", 2], ["power_left", "power", 2],
                    ["healing_per_mana", "heal/mana", 2], ["hot_healing_share", "hot", 2],
                    ["repeated_presses", "repeats", 1], ["hazard_seconds", "haz s", 1],
                    ["interruptible_casts_seen", "seen", 1], ["reward_interrupt", "r_int", 3]];
  document.getElementById("live").innerHTML = `
    <thead><tr><th>layout</th>${livecols.map(c => `<th>${c[1]}</th>`).join("")}</tr></thead><tbody>
    ${(LV.layouts || []).map(r => `<tr><td>${r.layout}</td>${livecols.map(c => {
        const v = r[c[0]];
        let k = "";
        if (c[0] === "killed") k = v >= 0.9 ? "good" : v < 0.7 ? "bad" : "warn";
        if (c[0] === "died" || c[0] === "timed_out") k = v <= 0.05 ? "good" : v > 0.2 ? "bad" : "warn";
        return `<td class="${k}">${v === undefined ? "-" : fmt(v, c[2])}</td>`;
      }).join("")}</tr>`).join("")}</tbody>`;

  const L = state.layouts || {};
  document.getElementById("layoutsteps").textContent = L.env_steps ? `(${steps(L.env_steps)} steps)` : "";
  const cols = [["killed", "kill", 2], ["died", "die", 2], ["timed_out", "t/o", 2],
                ["options_started", "opts", 1], ["option_seconds", "opt s", 1], ["seconds_per_option", "s/opt", 2],
                ["preparation_seconds", "prep", 1], ["in_melee_share", "melee", 2],
                ["repeated_presses", "repeats", 1], ["interrupts", "intr", 2],
                ["control_seconds", "control", 1], ["health_left", "hp", 2]];
  document.getElementById("layouts").innerHTML = `
    <thead><tr><th>layout</th>${cols.map(c => `<th>${c[1]}</th>`).join("")}</tr></thead><tbody>
    ${(L.layouts || []).map(r => `<tr><td>${r.layout}</td>${cols.map(c => {
        const v = r[c[0]];
        let k = "";
        if (c[0] === "killed") k = v >= 0.9 ? "good" : v < 0.7 ? "bad" : "warn";
        if (c[0] === "died" || c[0] === "timed_out") k = v <= 0.05 ? "good" : v > 0.2 ? "bad" : "warn";
        return `<td class="${k}">${fmt(v, c[2])}</td>`;
      }).join("")}</tr>`).join("")}</tbody>`;

  document.getElementById("runs").innerHTML = `
    <thead><tr><th>run</th><th>state</th><th>progress</th><th>best</th><th>baseline</th><th>updated</th></tr></thead><tbody>
    ${(state.runs || []).map(r => { const q = r.progress || {};
      const of = q.total_env_steps || 0, at = q.env_steps || 0;
      return `<tr class="${r.dir === state.current ? "current" : ""}">
      <td><span class="dot" style="background:${r.live ? "#5bd6a0" : "#3a4150"}"></span>${r.name}
          ${r.fast ? `<span class="tag">fast</span>` : ""}</td>
      <td>${r.finished ? (r.finished.reason || "finished") + (r.finished.advanced ? ", advanced" : "") : q.phase || "-"}</td>
      <td>${steps(at)}${of ? ` <span class="muted">/ ${steps(of)}</span>` : ""}</td>
      <td>${fmt(q.best_score, 2)}</td><td>${fmt(q.baseline_score, 2)}</td>
      <td class="muted">${q.updated_at ? new Date(q.updated_at * 1000).toLocaleTimeString() : "-"}</td></tr>`; }).join("")}
    </tbody>`;

  renderControls(run, p);
  renderSeed();
  renderConfig();
}

// The buttons the sim will actually accept right now. `forge resume` continues a paused or cancelled plan, so it
// is the one that makes sense while nothing is running; the rest need a live plan. Offering a button that is
// going to be refused is worse than not offering it.
function renderControls(run, p) {
  const names = state.controls || [];
  const box = document.getElementById("controls");
  if (!names.length) {
    box.innerHTML = `<span class="muted" title="start the dashboard with --soap-auth to enable these">read-only</span>`;
    return;
  }
  const phase = (run && !run.finished && p.phase) ? p.phase : "";
  const running = phase === "training" || phase === "evaluating";
  const enabled = { pause: running, resume: !running, skip: running, cancel: running };
  const label = { pause: "pause", resume: "resume", skip: "skip stage", cancel: "cancel" };
  box.innerHTML = names.map(n =>
    `<button data-cmd="${n}" class="${n === "cancel" ? "danger" : ""}" ${enabled[n] ? "" : "disabled"}
      >${label[n] || n}</button>`).join("");
  box.querySelectorAll("button").forEach(b => b.addEventListener("click", () => send(b.dataset.cmd)));
}

// Which checkpoint the stage after this one starts from. best.pt is only rewritten by an evaluation that clears
// the convergence margin, so it can sit a long way behind latest.pt -- and the next stage takes best.pt unless
// told otherwise, which silently throws that distance away.
function renderSeed() {
  const s = state.seed || {};
  const box = document.getElementById("seed");
  if (!s.resolved && !s.has) { box.innerHTML = `<span class="muted">no run yet</span>`; return; }

  const active = s.choice || "";
  const behind = s.behind || 0;
  const warn = (s.resolved === "best") && behind > 0;
  const why = !s.has || !s.has.best
    ? `<span class="muted why">no best.pt yet, so latest.pt is what seeds either way</span>`
    : warn
      ? `<span class="why warn">best.pt is ${steps(behind)} steps behind latest.pt${
          s.best_env_steps ? ` (best at ${steps(s.best_env_steps)}, now ${steps(s.env_steps)})` : ""}</span>`
      : `<span class="why good">best.pt is current</span>`;

  box.innerHTML = `<div class="seedrow">
      <span class="muted">next stage seeds from</span>
      <button data-seed="best" class="${active === "best" ? "on" : ""}">best.pt</button>
      <button data-seed="latest" class="${active === "latest" ? "on" : ""}">latest.pt</button>
      <button data-seed="default" class="${active === "" ? "on" : ""}"
              title="whatever the learner's own seed_from setting says">default</button>
      <span class="muted">&rarr; ${s.resolved ? s.resolved + ".pt" : "nothing"}</span>
      ${why}
    </div>`;
  box.querySelectorAll("button").forEach(b =>
    b.addEventListener("click", () => setSeed(b.dataset.seed)));
}

async function setSeed(choice) {
  const said = document.getElementById("said");
  said.className = "muted said";
  said.textContent = "setting...";
  try {
    const r = await fetch("api/seed", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ choice }),
    });
    const out = await r.json();
    said.className = "said " + (out.ok ? "good" : "bad");
    said.textContent = out.output || "";
  } catch (e) {
    said.className = "said bad";
    said.textContent = "could not set it";
  }
  poll();
}

async function send(command) {
  // Cancel stops the stage and the learner saves on its way out; skip abandons the rest of this stage's budget.
  // Both are a keystroke away from being a mistake, so they ask first.
  if ((command === "cancel" || command === "skip") &&
      !confirm(`${command} the current stage?`)) return;

  const said = document.getElementById("said");
  said.className = "muted said";
  said.textContent = `${command}...`;
  document.querySelectorAll("#controls button").forEach(b => b.disabled = true);
  try {
    const r = await fetch("api/command", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ command }),
    });
    const out = await r.json();
    said.className = "said " + (out.ok ? "good" : "bad");
    // The console answers in whole paragraphs, CRLF and all; the header has room for the first line of it and
    // the rest goes in the tooltip.
    const text = (out.output || "").replace(/\r/g, "");
    said.textContent = text.split("\n")[0].slice(0, 160) || (out.ok ? "done" : "refused");
    said.title = text;
  } catch (e) {
    said.className = "said bad";
    said.textContent = "the dashboard could not reach the sim";
  }
  poll();
}

function renderConfig() {
  const needle = document.getElementById("filter").value.toLowerCase();
  const only = document.getElementById("changedonly").checked;
  const rows = (state.config || []).filter(c =>
    (!only || c.changed) &&
    (!needle || c.key.toLowerCase().includes(needle) || String(c.value).toLowerCase().includes(needle)));
  document.getElementById("config").innerHTML = `
    <thead><tr><th>key</th><th>value</th><th>dist default</th></tr></thead><tbody>
    ${rows.map(c => `<tr class="${c.changed ? "changed" : ""}"><td>${c.key}</td><td>${c.value}</td>
      <td class="muted">${c.default === undefined || c.default === null ? "-" : c.default}</td></tr>`).join("")}
    </tbody>`;
}

async function poll() {
  try {
    const r = await fetch("api/state");
    state = await r.json();
    render();
  } catch (e) { document.getElementById("clock").textContent = "disconnected"; }
}
document.getElementById("filter").addEventListener("input", renderConfig);
document.getElementById("changedonly").addEventListener("change", renderConfig);
poll();
setInterval(poll, 5000);
</script>
</body></html>
"""


class Handler(BaseHTTPRequestHandler):
    runs_dir = DEFAULT_RUNS
    conf_path = DEFAULT_CONF
    soap_url = DEFAULT_SOAP_URL
    soap_auth: tuple[str, str] | None = None

    def _send(self, body: bytes, content_type: str):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802 - BaseHTTPRequestHandler's name
        path = self.path.split("?")[0].rstrip("/") or "/"
        if path in ("/", "/index.html"):
            self._send(PAGE.encode(), "text/html; charset=utf-8")
        elif path == "/api/state":
            state = collect(self.runs_dir, self.conf_path)
            # The page hides the buttons rather than offering ones that cannot work.
            state["controls"] = sorted(COMMANDS) if self.soap_auth else []
            payload = json.dumps(state).encode()
            self._send(payload, "application/json")
        else:
            self.send_error(404)

    def do_POST(self):  # noqa: N802 - BaseHTTPRequestHandler's name
        route = self.path.split("?")[0].rstrip("/")
        # Choosing the seed checkpoint writes a file rather than talking to the sim, so it works without SOAP.
        if route == "/api/seed":
            self._set_seed()
            return
        if route != "/api/command":
            self.send_error(404)
            return
        if not self.soap_auth:
            self._send(json.dumps({"ok": False, "output": "no SOAP credentials: the page is read-only"}).encode(),
                       "application/json")
            return

        try:
            length = int(self.headers.get("Content-Length") or 0)
            name = (json.loads(self.rfile.read(length) or b"{}") or {}).get("command", "")
        except (ValueError, TypeError):
            name = ""

        ok, output = run_command(str(name), self.soap_url, self.soap_auth)
        self._send(json.dumps({"ok": ok, "output": output}).encode(), "application/json")

    def _set_seed(self):
        """Write (or clear) the current run's seed_from marker.

        The only thing this page ever writes, and it writes one word into one file, in the run directory it is
        already showing. The directory comes from the server's own scan rather than from the request, so a request
        cannot name a path; the word is checked against SEED_CHOICES before it is written."""
        try:
            length = int(self.headers.get("Content-Length") or 0)
            choice = str((json.loads(self.rfile.read(length) or b"{}") or {}).get("choice", "")).strip().lower()
        except (ValueError, TypeError):
            choice = ""
        if choice not in (*SEED_CHOICES, "default"):
            self._send(json.dumps({"ok": False, "output": f"{choice or 'that'} is not a choice"}).encode(),
                       "application/json")
            return

        state = collect(self.runs_dir, self.conf_path)
        if not state.get("current"):
            self._send(json.dumps({"ok": False, "output": "no run to set it on"}).encode(), "application/json")
            return

        marker = Path(state["current"]) / SEED_MARKER
        try:
            if choice == "default":
                marker.unlink(missing_ok=True)
                said = "cleared: the learner's own setting decides"
            else:
                marker.write_text(choice + "\n")
                said = f"the next stage will seed from {choice}.pt"
        except OSError as error:
            self._send(json.dumps({"ok": False, "output": f"could not write it ({error.strerror})"}).encode(),
                       "application/json")
            return

        self._send(json.dumps({"ok": True, "output": said}).encode(), "application/json")

    def log_message(self, *_args):
        pass        # a poll every 5 s would fill the terminal it runs in


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8800)
    parser.add_argument("--host", default="127.0.0.1", help="loopback by default: the page is not authenticated")
    parser.add_argument("--runs", type=Path, default=DEFAULT_RUNS, help="AnimusForge.OutputDir's runs directory")
    parser.add_argument("--conf", type=Path, default=DEFAULT_CONF, help="the worldserver.conf (its AnimusForge.* keys)")
    parser.add_argument("--soap-url", default=DEFAULT_SOAP_URL, help="the sim's SOAP endpoint (SOAP.IP/SOAP.Port)")
    parser.add_argument("--soap-auth", type=Path, default=None,
                        help="file holding `user:password` for SOAP; without it the stage controls are hidden "
                             "and the page stays read-only")
    args = parser.parse_args()

    Handler.runs_dir = args.runs
    Handler.conf_path = args.conf
    Handler.soap_url = args.soap_url
    Handler.soap_auth = read_soap_auth(args.soap_auth)
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    controls = "stage controls on" if Handler.soap_auth else "read-only (no --soap-auth)"
    print(f"Animus Forge dashboard on http://{args.host}:{args.port}  (runs {args.runs}, {controls})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
